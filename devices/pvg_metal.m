/* Runs the guest's GPU commands on the host's Metal, on any Apple platform.
   The guest driver serializes Metal calls: an exec names command buffers in a
   task's memory, each a segment holding {tag, size} records, one per call, in
   the formats worked out from the guest driver and listed by the decoders
   below. Objects are named by a number (a ref); the task's object list says
   what each is, and they are made the first time they are used. */

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <stdatomic.h>
#include <os/lock.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <TargetConditionals.h>
#import <IOSurface/IOSurfaceRef.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/sysctl.h>

static inline BOOL cursor_sharp_cached(void) {
    static int cached = -1;
    static uint64_t last_check = 0;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (cached < 0 || (now - last_check) > 1000000000ull) { /* refresh at most once a second */
        last_check = now;
        NSNumber *s = [[NSUserDefaults standardUserDefaults] objectForKey:@"henge.cursor.sharp"];
        cached = s ? s.boolValue : 1;
    }
    return cached != 0;
}

/* 1080p shrunk onto a phone would make a normal pointer a few dots across; an iPad's screen needs less. */
static int cursor_scale(void) {
#if TARGET_OS_IPHONE
    static int cached_scale = -1;
    static uint64_t last_scale_check = 0;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (cached_scale > 0 && (now - last_scale_check) <= 1000000000ull) {
        return cached_scale;
    }
    last_scale_check = now;
    NSInteger saved = [[NSUserDefaults standardUserDefaults] integerForKey:@"henge.cursor.scale"];
    if (saved >= 1 && saved <= 3) {
        cached_scale = (int)saved;
        return cached_scale;
    }
    static int defaultScale;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        struct utsname u;
        uname(&u);
        defaultScale = strncmp(u.machine, "iPad", 4) ? 3 : 1;
    });
    cached_scale = defaultScale;
    return cached_scale;
#else
    return 1;
#endif
}

#include <mach/mach.h>
#include <stdatomic.h>
#include <mach/vm_map.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "frame_pace.h"
#include "pvg_lite.h"
#include "gpu_switch.h"
#include "pvg_command.h"
#include "pvg_validate.h"
#include "pvg_video.h"
#include "ramfile.h"
#include "metallib.h"

#if !TARGET_OS_IPHONE
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#endif

/* Metal's own pipeline serialization, on _MTLDevice. The guest's Metal wrote the data. */
@protocol HvMetalSPI
- (id)newRenderPipelineDescriptorWithSerializedData:(dispatch_data_t)d deserializationContext:(id)c;
- (id)newTileRenderPipelineDescriptorWithSerializedData:(dispatch_data_t)d deserializationContext:(id)c;
- (id)newComputePipelineDescriptorWithSerializedData:(dispatch_data_t)d deserializationContext:(id)c;
@end

enum { OBJ_BUFFER = 1, OBJ_TEXTURE = 2, OBJ_FUNCTION = 6, OBJ_SERIALIZED = 7, OBJ_LINEAR_TEXTURE = 8, OBJ_MAPPED_TEXTURE = 0xb };

/* What an object was made as. Metal takes any id where it wants a buffer, a texture, a sampler or a pipeline, and one
   of another kind stops the app or is read as what it is not: every use asks for the kind it wants (object:task:kind:),
   and a ref naming anything else gives nil. */
typedef enum { KIND_NONE, KIND_BUFFER, KIND_TEXTURE, KIND_SAMPLER, KIND_RENDER_PIPELINE, KIND_TILE_PIPELINE, KIND_COMPUTE_PIPELINE,
               KIND_DEPTH_STENCIL, KIND_FENCE, KIND_HEAP, KIND_FUNCTION, KIND_COUNT } HvKind;
static const char *const kind_names[KIND_COUNT] = {"nothing", "buffer", "texture", "sampler", "render pipeline", "tile pipeline",
                                                    "compute pipeline", "depth-stencil state", "fence", "heap", "function"};

/* What a render pass draws into, and what a pipeline was made to draw into: Metal takes a pipeline only in a pass of the
   same (and stops the app, or draws to the wrong formats, otherwise). */
typedef struct {
    uint32_t color[8], depth, stencil, samples;
} HvTargets;

/* A multi-plane surface's host side: a real IOSurface laid out as the guest's planes, and what keeps the two the same
   (pvg_video.h). One for each guest memory, format and layout (HvReplay's planar), whatever mapping ids the guest's
   processes map it under: what one process draws (macOS 27's WebKit draws pages into b3a8 tiles in its GPU process) is
   what another reads (WindowServer), and every mapping of it agrees with the one shadow. It holds no guest memory:
   each mapping reaches the same guest pages through its own (an HvObject's mapped), so it can outlive them all without
   keeping a page in a save.
   plane: its planes in the guest's pages, each checked against alloc. shadow: the bytes guest and host last agreed on,
   as they went up or came back (laid out as the guest's). host_newer: an exec drew into it since the guest last wrote
   it (an exec's flag 1) or all the GPU drew came back to the guest's pages, so only the bytes the guest changes go up,
   over the drawing (hv_pvg_video_plane_merge), and it is copied back when the guest asks: write_gen counts the execs
   that draw into it (the worker's), back_gen is the newest of them whose drawing came back (or never will).
   merged: the rows those bytes went into (a bit a row, hv_pvg_video_row_bits), sent whole when the guest next says
   it wrote it or all the drawing counts as back, those that differ (merged_any: one is marked; the worker's alone).
   last_reader: the newest GPU work that uses it, until that is done. backs_due: the copy-backs of it that
   committed execs' completions are still to make (commit: counts each, its part on complete_q lets it go, copied
   or failed), and back_cb the newest of those execs: the worker lets them all be made before it has the GPU draw
   into it again (backs_wait), else one would copy the surface back half drawn over. video_lock: the IOSurface, shadow,
   guest pages, back_gen and back_cb, between the worker and the copy-backs on complete_q. reader_lock: last_reader
   alone, between the worker and Metal's completion handler, which clears it: never behind video_lock, which the
   worker holds for a whole surface's compare and upload (about 1 ms each for 4K, at every use), else the handler
   gives the slot back and wakes the presenter that much later. Neither is taken under the other. */
@interface HvVideo : NSObject {
@public
    IOSurfaceRef iosurface;
    uint64_t alloc;
    uint32_t plane_count;
    HvVideoPlane plane[4];
    uint8_t *shadow, *merged;
    BOOL host_newer, merged_any;
    uint64_t write_gen, back_gen;
    id<MTLCommandBuffer> last_reader, back_cb;
    _Atomic int backs_due;
    os_unfair_lock video_lock, reader_lock;
    uint64_t held; /* its IOSurface's and shadow's bytes (HvReplay's planar_kept) */
}
@end
@implementation HvVideo
- (void)dealloc {
    if (iosurface) CFRelease(iosurface);
    free(shadow), free(merged);
}
@end
/* The bytes of video surfaces no longer mapped that planar_kept may hold: only so a remap soon after shares what was
   drawn (a WebKit tile's IOSurface and shadow are a few MB; a 4K 4:2:0 frame's 25), so a little, and less on an iPad,
   where the app's whole memory is what jetsam allows and the guest's RAM takes most of it. Those still mapped cost
   nothing here: their mappings hold them, as each mapping held its own before they were shared. */
#ifndef HV_PVG_PLANAR_KEPT
#if TARGET_OS_IPHONE
#define HV_PVG_PLANAR_KEPT (128ull << 20)
#else
#define HV_PVG_PLANAR_KEPT (256ull << 20)
#endif
#endif
#define HV_PVG_PLANAR_KEPT_MAX 512 /* and how many */

#define HV_PVG_STAGE_RING 1024 /* commands queued for the worker whose queueing time is kept: the oldest of more is the
                                  1024th newest, an age too short only when that many wait */
static uint64_t stage_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
@class HvReplay;
static void gpu_begun(HvReplay *r);
static void presenter_take(HvReplay *r, id<MTLCommandBuffer> cb);
typedef enum { PICK_DRAW, PICK_AGAIN, PICK_STOP } PresenterPick;
static BOOL presenter_commit(HvReplay *r, uint64_t generation, CAMetalLayer *target, id<MTLCommandBuffer> cb,
                             id<CAMetalDrawable> drawable, uint64_t swap_ns, uint64_t held_from);
static PresenterPick presenter_pick(HvReplay *r, uint64_t generation, CAMetalLayer *target, uint32_t W, uint32_t H, BOOL fits,
                                    BOOL half, id<MTLCommandBuffer> cb, uint64_t *swap_ns);

/* What is known about a made object, so a reused ref is noticed. */
@interface HvObject : NSObject {
@public
    id object;
    HvKind kind; /* of object; KIND_NONE while it is nil */
    uint32_t entry[3];
    /* A buffer's guest pages, mapped contiguously. */
    uint8_t *mapped;
    BOOL mapping_in_buffer; /* the buffer over them frees them */
    BOOL plain_buffer; /* the object is that buffer */
    uint32_t generation; /* a new mapping since: what was made from it is out of date */
    HvObject *follows; /* made from this one's buffer */
    uint32_t follows_generation;
    uint64_t length;
    uint32_t vpage;
    uint32_t *pages; /* the guest's page numbers, as its table had them when mapped (map_pages) */
    BOOL pages_in_ram; /* some of them are guest RAM: the rest, outside it, are mapped as zeros */
    /* A texture's layout in guest memory. */
    uint32_t tex_mips, tex_slices;
    uint64_t tex_base, tex_stride;
    uint8_t tex_block_h;
    uint8_t *tex_layout; /* the mip table, 36 bytes each */
    BOOL guest_backed;
    uint32_t retries; /* a mapped texture made before its surface was described: tried again on later uses */
    uint8_t described[0x30]; /* a surface: the older driver's compact description it was made from */
    uint64_t on_mapping; /* a mapped texture: the surface id it is over */
    /* A surface: */
    HvObject *surface;
    /* A surface whose mapping the guest unmapped (hv_pvg_metal_unmap): its pages are the guest's to reuse once the
       mapper's fence is done, so nothing made over it is used again (object:task: makes it anew). On a mapped texture:
       refused for a mapping unmapped then (unmapped), and made anew on its next use. The worker's. */
    BOOL gone;
    /* A surface dropped by a map of its id (its unmap was never walked, so the mapper's fence never waited for its
       copy-backs) and no other mapping has its video: the copy-backs still to come through it are not made, for its
       pages may be the guest's other memory by now (video_back). Set by the worker, read on complete_q. */
    _Atomic int backs_off;
    /* What the guest has made over it (and their views), weakly: dropped from the cache when it is unmapped, for none
       is used again (entry:task:into:kept:), and each keeps it, its pages and their GPU mapping alive. */
    NSHashTable<HvObject *> *over;
    uint32_t cache_task, cache_ref; /* where this one is in tasks, for the surface it is over to find it */
    uint64_t row;
    uint32_t first;
    uint32_t surface_w, surface_h, fourcc; /* its own, whatever views the guest makes of it */
    id<MTLTexture> scanout; /* the same pixels, as the display reads them */
    /* Multi-plane surfaces cannot sit on a buffer: a real IOSurface, kept up to date with the guest's pages (through
       this one's mapped) */
    HvVideo *video;
    /* A heap: private memory, a buffer over all of it, and its bytes in heap_total until it goes. */
    id<MTLBuffer> heap_whole;
    _Atomic uint64_t *heap_total;
    uint64_t heap_size;
    /* A heap's or a private texture's copy in guest memory, and which side has the newest bytes. */
    id<MTLBuffer> guest_copy;
    BOOL host_valid, guest_stale;
    /* What this one is made from, kept for as long as it lives. */
    HvObject *parent;
    /* A render or tile pipeline: what it draws into (formats, samples), and its patches' control points (-1: it draws
       no patches; 0: any number) */
    HvTargets targets;
    int32_t patch_points;
    uint32_t tess_weight; /* a patch's vertices at most: its largest tessellation factor squared (1 for no patches) */
    uint32_t patch_rec; /* a patch's tessellation factors, bytes: 8 for triangles, 12 for quads (0 for no patches) */
    uint32_t patch_step; /* and how they are read (MTLTessellationFactorStepFunction: hv_pvg_tess_fit) */
    uint32_t patch_index; /* its control point indexes' bytes (tessellationControlPointIndexType): 2 or 4, 0 if it says neither */
}
@end

/* An indirect draw of a pass, as the pass's clamp (clampPass:) found it: where its arguments came from and what they
   were checked against. The draw takes them only from the clamped copy, and only with the same buffers. */
@interface HvIndirect : NSObject {
@public
    const uint8_t *at; /* the record, in the exec's copy of its command buffer */
    uint32_t seg; /* and the segment it is in (one copy may be two segments: the same range named twice) */
    uint32_t tag, kind, weight, limit; /* HvClampJob's */
    id<MTLBuffer> src, index, patches, points;
    uint64_t src_off;
    id<MTLBuffer> factors; /* a patch draw's tessellation factors, from foff with fstride, and its pipeline's record */
    uint64_t foff, fstride;
    uint32_t frec, fstep;
    uint32_t cpsize; /* 19: the control point indexes' bytes the clamp counted with (2 or 4) */
}
@end
@implementation HvIndirect
@end

@implementation HvObject
- (void)dealloc {
    if (mapped && !mapping_in_buffer) {
        hv_ramfile_gpu_unmap(mapped); /* first: once deallocated, a new mapping may get this address */
        vm_deallocate(mach_task_self(), (vm_address_t)mapped, (length + 0x3fff) & ~0x3fffull);
    }
    if (heap_total) atomic_fetch_sub(heap_total, heap_size);
    free(pages);
    free(tex_layout);
}
@end

/* A task's objects by ref. A ref is an index into the guest's own object list (list + ref * 12): a table by the ref
   itself, not a dictionary of boxed numbers, as each bind of each draw looks one up (gfx-cost 7). Open addressing,
   linear probing, at most half full: the memory it takes follows how many objects it holds (16 bytes a slot), never
   how far apart their refs are, which is the guest's to choose (any of 2^32). It shrinks as objects leave. */
#define HV_TASKS 512 /* pvg_lite.c keeps object lists for task numbers below this, and none other has objects */
#define HV_OBJS_MIN 16u
@interface HvTaskObjects : NSObject {
@public
    uint32_t *refs;
    HvObject *__strong *slots; /* nil: empty */
    uint32_t cap; /* a power of two, or 0 before the first */
    NSUInteger count;
}
@end

static inline uint32_t objs_home(uint32_t ref, uint32_t cap) { return (uint32_t)(ref * 0x9e3779b1u) & (cap - 1); }

static HvObject *objs_get(HvTaskObjects *t, uint32_t ref) {
    if (!t || !t->cap) return nil;
    for (uint32_t i = objs_home(ref, t->cap); t->slots[i]; i = (i + 1) & (t->cap - 1))
        if (t->refs[i] == ref) return t->slots[i];
    return nil;
}

/* Into new arrays of cap slots: NO if there is no memory (nothing changed) */
static BOOL objs_resize(HvTaskObjects *t, uint32_t cap) {
    uint32_t *refs = (uint32_t *)malloc(cap * sizeof *refs);
    HvObject *__strong *slots = (HvObject *__strong *)calloc(cap, sizeof(HvObject *));
    if (!refs || !slots) { free(refs), free((void *)slots); return NO; }
    for (uint32_t k = 0; k < t->cap; k++) {
        if (!t->slots[k]) continue;
        uint32_t i = objs_home(t->refs[k], cap);
        while (slots[i]) i = (i + 1) & (cap - 1);
        refs[i] = t->refs[k], slots[i] = t->slots[k];
        t->slots[k] = nil;
    }
    free(t->refs), free((void *)t->slots);
    t->refs = refs, t->slots = slots, t->cap = cap;
    return YES;
}

/* NO if there is no memory for it (nothing changed) */
static BOOL objs_set(HvTaskObjects *t, uint32_t ref, HvObject *o) {
    uint32_t i = t->cap ? objs_home(ref, t->cap) : 0;
    while (t->cap && t->slots[i] && t->refs[i] != ref) i = (i + 1) & (t->cap - 1);
    BOOL there = t->cap && t->slots[i];
    if (o && there) { t->slots[i] = o; return YES; }
    if (o) {
        if ((t->count + 1) * 2 > t->cap) {
            if (t->cap >= 1u << 31 || !objs_resize(t, t->cap ? t->cap * 2 : HV_OBJS_MIN)) return NO;
            for (i = objs_home(ref, t->cap); t->slots[i]; i = (i + 1) & (t->cap - 1)) {}
        }
        t->refs[i] = ref, t->slots[i] = o, t->count++;
        return YES;
    }
    if (!there) return YES;
    /* Out, and those after it moved back over the gap (no tombstones): each stays reachable from its home */
    t->slots[i] = nil, t->count--;
    for (uint32_t j = (i + 1) & (t->cap - 1); t->slots[j]; j = (j + 1) & (t->cap - 1)) {
        uint32_t home = objs_home(t->refs[j], t->cap);
        if (((j - home) & (t->cap - 1)) < ((j - i) & (t->cap - 1))) continue; /* its home is after the gap: stays */
        t->refs[i] = t->refs[j], t->slots[i] = t->slots[j], t->slots[j] = nil;
        i = j;
    }
    if (t->cap > HV_OBJS_MIN && t->count * 8 < t->cap) objs_resize(t, t->cap / 2); /* no memory: stays as big */
    return YES;
}

/* Every object, in no particular order: for (at = 0; (o = objs_next(t, &at, &ref)); at++). at is a place in the table,
   not a ref; nothing may be set or taken out during the walk. */
static HvObject *objs_next(HvTaskObjects *t, uint32_t *at, uint32_t *ref) {
    for (; t && *at < t->cap; ++*at)
        if (t->slots[*at]) {
            if (ref) *ref = t->refs[*at];
            return t->slots[*at];
        }
    return nil;
}

@implementation HvTaskObjects
- (void)dealloc {
    for (uint32_t k = 0; k < cap; k++) slots[k] = nil;
    free(refs), free((void *)slots);
}
/* for the tests: objs[@(ref)] */
- (HvObject *)objectForKeyedSubscript:(NSNumber *)ref { return objs_get(self, ref.unsignedIntValue); }
- (void)setObject:(HvObject *)o forKeyedSubscript:(NSNumber *)ref { objs_set(self, ref.unsignedIntValue, o); }
- (void)removeObjectForKey:(NSNumber *)ref { objs_set(self, ref.unsignedIntValue, nil); }
@end

/* o is over a surface (o->surface): the surface keeps track of it (over). */
static void over_surface(HvObject *o) {
    HvObject *s = o->surface;
    if (!s) return;
    if (!s->over) s->over = [NSHashTable weakObjectsHashTable];
    [s->over addObject:o];
}

@class HvContext;

@interface HvReplay : NSObject {
@public
    HvPvgLite *lite;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    dispatch_queue_t worker;
    HvTaskObjects *tasks[HV_TASKS]; /* by task number; nil for one with nothing made */
    /* Per task, the host addresses and resource IDs told to the guest (info 460...468), by where they were
       written: a process that restores a snapshot has others, and tells the guest again (snap_reanswer). */
    NSMutableDictionary<NSNumber *, NSMutableDictionary<NSData *, NSData *> *> *answers;
    uint32_t answers_count;
    /* An answer that couldn't be kept (SNAP_MAX_ANSWERS of them still in use): a save then fails, and the Mac
       stays paused, rather than going on in another process with a host value it was never told again. */
    BOOL answers_lost;
    NSMutableDictionary<NSNumber *, HvObject *> *surfaces;
    /* Mapping ids the guest unmapped and has not mapped again (the mapper's ops 2 and 1), a bit each: their descriptors
       may still name pages it has since reused, so no surface is made from one (makeMappedTexture). Ids are 1 to 0x1fff
       (the mapper refuses any other, as Apple's host does: pvg_lite.c's mapping_id). The mapper's own state is the
       whole of it (hv_pvg_metal_set_unmapped), taken when this is made and so on a restore; kept in step here in the
       ring's order after that. The worker's. */
    uint8_t unmapped[HV_PVG_MAPPING_IDS / 8];
    uint64_t stale_dropped; /* surfaces dropped by a map of their id (hv_pvg_metal_map): their unmap was passed over */
    /* Multi-plane surfaces' host sides by the guest memory they stand for (video_key): each process maps a surface
       under its own mapping id, and what one draws (macOS 27's WebKit draws pages into b3a8 tiles in its GPU
       process) has to be what another reads (WindowServer). Weak: one is found for as long as anything has it (its
       mappings, textures over them, work in flight), so a surface still in use is never split in two, and none is
       held here (its mappings pay for it). planar_kept: those whose mapping went most recently (oldest first), held
       so a process that maps one soon after still finds what was drawn, no more than HV_PVG_PLANAR_KEPT bytes and
       HV_PVG_PLANAR_KEPT_MAX of them (planar_keep). The worker's only. */
    NSMapTable<NSData *, HvVideo *> *planar;
    NSUInteger planar_sweep; /* planar's count when it is next rid of the keys of surfaces gone */
    NSMutableArray<HvVideo *> *planar_kept;
    uint64_t planar_kept_bytes;
    NSMutableSet<NSString *> *complaints;
    BOOL complaints_full;
    uint32_t libs_many; /* libraries refused for holding other than one function, logged at 1, 10, 100... (worker) */
    uint32_t tasks_forgot[2]; /* tasks deleted, and defined again before a delete, that held anything: logged at 1, 10, 100... (worker) */
    uint64_t execs, draws, synced, remaps, remaps_failed;
    uint64_t pages_misaligned; /* a surface's guest pages not on a 16 KB boundary: zeros, never shifted bytes (S3-2) */
    /* An exec's resources' objects (exec), kept here, not freed at its end: a throw out of the exec (caught in
       hv_pvg_metal_submit; ARC doesn't release on one) leaves them held only until the next exec clears them. */
    HvObject *__strong *exec_found;
    uint32_t exec_found_cap, exec_found_n;
    _Atomic uint64_t failed; /* also counted from completion handlers, on Metal's threads */
    /* What this GPU can do: Metal stops the app on a description or a command past it (pvg_validate.h, pvg_command.h) */
    HvTexCaps caps;
    HvCmdLimits limits;
    uint64_t max_buffer; /* maxBufferLength: no guest length maps or allocates more */
    uint64_t heap_limit; /* the heaps' bytes, all told (recommendedMaxWorkingSetSize) */
    _Atomic uint64_t *heap_total; /* never freed: a heap can outlive this */
    /* The pipeline in the open encoder, for what its dispatches may ask */
    id<MTLRenderPipelineState> cur_render;
    id<MTLComputePipelineState> cur_compute;
    /* Mapper fences complete here (hv_pvg_lite_mapper_done takes the device lock): never on a thread the device
       lock's holder may wait for, such as the GPU's completion handlers while a save waits for the GPU. */
    dispatch_queue_t mapper_q;
    /* The guest's side of the GPU's completions (stamps, video copy-backs, mapper fences): run here, one at a time in
       the order of commit, never on Metal's completion threads. They write the guest's pages, which save-ahead may be
       writing out to their file: a store there can wait for that write (seconds, at its throttled I/O), and Metal runs
       the app's completion handlers one after another, so the iPad's own drawing would wait with it. Never takes the
       device lock either (as mapper_q). late_told: the last "late completion" line's second (late_probe). */
    dispatch_queue_t complete_q;
    _Atomic int64_t late_told;
    /* complete_q's work not yet done, every block from the commit of its command buffer: once it is 0 with the worker
       parked, nothing writes the guest's pages through a GPU mapping (hv_pvg_metal_hold) */
    _Atomic int guest_side;
    /* The guest's command buffers committed and not yet done on the GPU (commit:, down in Metal's own completion
       handler). What Try Again waits out (gpuState): only these can hang. guest_side counts more: the empty trailing
       buffers (startTrail), which can't hang but wait behind the presenter's frames, and complete_q's work after. */
    _Atomic int guest_on_gpu;
    /* Video surfaces the GPU drew into that the guest asked back: read back once this exec's GPU work is done, one
       mapping of each (back_once) */
    NSMutableArray<HvObject *> *writeback;
    /* The video surfaces the next commit's work uses (exec's): it is their last_reader until it is done */
    NSArray<HvVideo *> *readers;
    uint64_t video_uploads, video_rows, video_waits, video_backs, video_back_rows; /* the backs' from completions: tallies only */
    uint64_t video_merges; /* uploads of the guest's bytes onto the GPU's drawing not yet back (sync_iosurface) */
    uint64_t video_unsaid; /* uploads at an exec's use with no flag 1: what macOS wrote without saying (evidence for any
                              later "skip the unchanged compare" design, such as dirty-page tracking) */
    double video_back_time; /* how long the copy-backs held their completions (and every stamp behind them): tallies only */
    /* The environment's debugging switches, read once (read_env): getenv takes about 100 ns, and some of these were
       read for each resource of each exec, each function, pipeline, heap, video sync and frame (gfx-cost 7) */
    struct {
        const char *trace_file, *host_dump, *surface_dump;
        BOOL fail_libs, shader_dump, heap_log, tally, cursor_log, gpu_test, color_probe;
    } env;
    _Atomic uint64_t gpu_waits_given_up; /* cb_wait past its bound: what wanted the buffer went without */
    id<MTLCommandBuffer> stuck_cb; /* the last buffer cb_wait gave up on, while it is still not done (worker only) */
    /* Video surfaces before an exec: compared with what went up last (scans, their time), the changed rows put in
       the IOSurface (uploads, their time), and the waits for the GPU still reading one (video_waits, their time):
       apart, so the stats line says which costs. The worker's. */
    uint64_t video_scans;
    uint64_t video_bytes_compared; /* bytes of video surfaces compared with their shadow: the guest's side (as many again read of the shadow) */
    uint64_t surfaces_made; /* surfaces made for the guest's mappings, and their shapes said so far (one line each) */
    NSMutableSet<NSData *> *surface_shapes;
    double video_scan_time, video_upload_time, video_wait_time;
    /* GPU code this OS refused (libraries, even relabelled, and pipelines from relabelled ones), and libraries it took
       only relabelled (metallib.h). */
    _Atomic uint32_t libs_refused, libs_lowered;
    /* Work the GPU has not finished, and the guest's work until its completion's part on complete_q is done too.
       Stamps are given back in the order they came: the GPU's completion handlers run in the order of commit, and
       complete_q runs what they hand it in that order. */
    _Atomic int inflight;
    /* For the freeze recorder (hv_pvg_metal_stages), ns of CLOCK_UPTIME_RAW: when the commands handed to the worker
       (hv_pvg_metal_submit) were queued, the latest HV_PVG_STAGE_RING by number, with how many were handed over and
       how many it has begun; when the worker began the one it runs (0: none); and when GPU work last finished, or
       began with none in flight (read while inflight > 0; a buffer's part on complete_q counts as in flight, so this
       stage includes complete_q's time). The worker's other blocks (maps, unmaps, the screen) are
       not commands: one of them held up shows as commands queued and none run. And since when the presenter has
       wanted a drawable and not had one (its waits and retries; 0 once it has one, gives up for a cool-down, or the layer
       goes). */
    _Atomic uint64_t st_queued[HV_PVG_STAGE_RING];
    _Atomic uint64_t st_submitted, st_started, st_worker_since, st_gpu_since, st_drawable_since;
    id<MTLCommandBuffer> last_cb;
    /* The newest empty buffer finishChannel or afterGPU committed, while nothing of the guest's was committed after it
       and its part on complete_q is still to run: what either wants after the GPU's work so far joins its list (trail_then)
       instead of a buffer each (joinTrail:). Each buffer counts against the queue's 64, and [queue commandBuffer] waits
       for room, so one each, without bound, could stall the worker. trail_presents: the presenter's commits (presents)
       when it was committed: afterGPU joins only if the presenter committed nothing since. trail_lock: the worker and
       complete_q. */
    id<MTLCommandBuffer> trail;
    NSMutableArray *trail_then;
    uint64_t trail_presents;
    os_unfair_lock trail_lock;
    _Atomic uint64_t presents; /* the presenter's commits, under screen_lock */
    /* At most HV_PVG_GUEST_INFLIGHT of the guest's command buffers committed and not done (see commit:) */
    dispatch_semaphore_t guest_slots;
    /* The GPU failed the guest's work (gpuFailed:): none of its work is run again in this run of the VM, until the
       app re-arms it (hv_pvg_metal_gpu_rearm). Its execs are still taken, and their stamps given back, so the guest
       goes on. Not in a snapshot, on purpose: a Mac resumed from Suspend is a new process with it clear, which is the
       other way it is re-armed. strikes: when the last failures that don't trip it at once came (gpu_strike_lock). */
    _Atomic int gpu_faulted;
    _Atomic int gpu_tries; /* how many times the person has had it run again (gpuTryAgain), of HV_GPU_TRIES */
    _Atomic int reupload; /* tried again: every guest-backed object's bytes go up again when next used (exec:) */
    /* An auto-save's hold on the worker (hv_pvg_metal_hold): 0 none, 1 held, 2 held but the worker went on (it waited
       HV_PVG_HOLD_MAX). release: what lets the parked worker go. */
    _Atomic int held;
    dispatch_semaphore_t hold_release;
    os_unfair_lock gpu_strike_lock;
    uint64_t gpu_strikes[3];
    uint32_t gpu_strike_next;
    double encode_time, gpu_wait_time;
    /* Of the same 2,000 execs: the longest one's encoding, and the pipelines built (the back-end compile, on this
       thread, every exec behind it waiting) and how long they took. Whether they are the hitches on an app's first
       use, and on later runs, is what says if a pipeline archive across runs is worth having (gfx-cost 3) */
    double encode_max, pipeline_time, pipeline_max;
    uint64_t pipelines_built;
    uint64_t heap_buffers_refused; /* heap buffers placed where the heap can't hold them: none made (makeHeapBuffer) */
    BOOL pipeline_set; /* in the open encoder */
    BOOL pipeline_tile; /* and it is a tile pipeline: no draws, only tile dispatches */
    int32_t pipeline_patches; /* its control points (-1: no patches; 0: any) */
    uint32_t pipeline_tess; /* its patches' weight (HvObject's tess_weight) */
    /* This exec's work so far (hv_pvg_command_work) and whether it went past HV_PVG_EXEC_WORK; the GPU's budget for
       its indirect draws (made with its first); the open pass's indirect draws, their clamped arguments, and the
       next one's place */
    uint64_t exec_work;
    uint32_t exec_indirect; /* this exec's indirect draws clamped so far: no more than HV_PVG_EXEC_INDIRECT */
    BOOL exec_over;
    id<MTLBuffer> exec_left;
    NSArray<HvIndirect *> *pass_indirect;
    id<MTLBuffer> pass_args;
    uint32_t indirect_next, indirect_at;
    id<MTLComputePipelineState> clamp_pipe;
    BOOL clamp_tried;
    NSData *clamp_jobs_last; /* for tests: the last pass's jobs, and the indirect draws drawn from clamped arguments */
    uint64_t indirect_drawn;
    HvTargets pass_targets; /* the open pass's */
    BOOL tess_factors; /* so is a tessellation factor buffer */
    id<MTLBuffer> tess_buf; /* it (render 122), its offset (within it) and instance stride */
    uint64_t tess_off, tess_stride;
    uint32_t pipeline_frec, pipeline_fstep; /* the pipeline's patches' factor records (HvObject's patch_rec, patch_step) */
    uint32_t pipeline_cpsize; /* and their control point indexes' bytes, 2 or 4 (HvObject's patch_index; 4 if it says neither) */
    uint64_t pass_w, pass_h, pass_layers; /* the open pass's render target: scissors stay within it */
    uint64_t bound[3][31]; /* lengths of the buffers bound (vertex or compute, fragment, tile), 0 for none */
    uint32_t pre_raster_samples, pre_imageblock, pre_threadgroup, pre_tile_w, pre_tile_h; /* for the next pass */
    BOOL tracing; /* HV_PVG_TRACE_FILE exists: say what each exec draws */
    MTLRenderPassDescriptor *pass_desc; /* recycled render pass descriptor */
    /* The newest frame, BGRA, for hv_pvg_metal_frame, and the same without the pointer on it. The worker's;
       frame_lock is held while it changes frame, frame_w, frame_h or frame's pixels, for hv_pvg_metal_frame copies
       them from other threads (the boot screen, a snapshot). */
    uint8_t *frame, *base;
    uint32_t frame_w, frame_h;
    os_unfair_lock frame_lock;
    /* The pointer: macOS sends it apart from the screen, for the display hardware to overlay. */
    uint8_t *cursor;
    uint32_t cursor_w, cursor_h, cursor_stride;
    int cursor_hot_x, cursor_hot_y;
    BOOL cursor_shown;
    _Atomic int cursor_ever_shown; /* macOS has shown its pointer: WindowServer is up (the login window or the desktop) */
    BOOL pointer_hidden; /* the host's choice, under screen_lock: hidden while a finger is the pointer */
    int drawn_x, drawn_y, drawn_w, drawn_h; /* where it is on the frame now */
    double last_capture;
    /* A frame that came too soon after the last: shown shortly, for it may be the last for a long while. */
    uint64_t late_mapping;
    BOOL late, late_queued;
    uint64_t frames, frames_shot;
    /* Showing frames with the GPU: a layer the app gave us. No copies, no conversion on a CPU. */
    CAMetalLayer *layer; /* screen_lock protects the target and its generation */
    uint64_t layer_generation;
    __weak CAMetalLayer *prepared_layer; /* presenter only: each target needs its own setup */
    id<MTLRenderPipelineState> screen_pipe, linear_pipe, cursor_pipe;
    id<MTLFunction> stand_in; /* computeStandIn */
    id<MTLTexture> cursor_tex;
    id<MTLSamplerState> screen_sampler, cursor_sampler;
    /* presenter only: fitting a screen of another size to the layer's pixels (resample_h into resample_mid, then resample_v) */
    id<MTLRenderPipelineState> resample_h8, resample_h16, resample_v;
    id<MTLTexture> resample_mid;
    MTLRenderPassDescriptor *screen_pass, *resample_pass;
    uint32_t resample_logged_w, resample_logged_h;
    uint64_t on_screen; /* the mapping showing */
    _Atomic uint64_t gpu_frames;
    /* each present's hold on the guest's GPU work (from presenter_take to its commit), in all and the longest, since the
       stats line last said them: the presenter adds, the worker takes them */
    _Atomic uint64_t present_holds, present_hold_ns, present_hold_max_ns;
    dispatch_queue_t presenter;
    os_unfair_lock screen_lock; /* what to show, and the pointer as the presenter reads it */
    id<MTLTexture> screen_tex;
    BOOL screen_half, screen_dirty, presenter_running, presenter_drawing;
    NSMutableArray *present_then; /* work waiting for the presenter's frame to finish */
    uint64_t swap_waiting_ns; /* the Mac's newest picture not yet taken to the screen: when it was swapped (0: none) */
    BOOL drawable_retry_pending;
    double drawable_retry_cooldown; /* fresh work coalesces one wake at its expiry */
    uint64_t screen_requests; /* fresh frames/cursor changes, not automatic retries */
    double drawable_retry_deadline; /* at most two seconds of retrying an unavailable target */
    uint32_t screen_w, screen_h;
    id<MTLBuffer> test_readback; /* HV_PVG_GPU_TEST: the drawable's pixels for screenshots */
    id<MTLBuffer> readback;
    const char *shot;
    unsigned shots;
    dispatch_source_t shot_timer;
}
- (void)complain:(NSString *)what;
- (HvObject *)newSurface:(uint64_t)mapping;
- (HvObject *)currentSurface:(uint64_t)mapping;
- (HvObject *)object:(uint32_t)ref task:(uint32_t)task;
- (HvObject *)object:(uint32_t)ref task:(uint32_t)task kind:(HvKind)kind;
- (void)drawCursor;
- (void)drawCursorLocked;
- (BOOL)hasDisplayLayer;
- (void)kickPresenter:(BOOL)fresh;
- (void)present:(uint64_t)mapping;
- (void)present:(uint64_t)mapping swappedAt:(uint64_t)swap_ns;
- (void)keepWaitingSwap:(uint64_t)swap_ns;
- (void)drainPresentThen;
- (void)screenChanged;
@end

/* A task's objects, its table made if asked for; nil past HV_TASKS */
static HvTaskObjects *task_objects(HvReplay *r, uint32_t task, BOOL make) {
    if (task >= HV_TASKS) return nil;
    if (!r->tasks[task] && make) r->tasks[task] = [HvTaskObjects new];
    return r->tasks[task];
}

/* A count to log at: 1, 10, 100... A guest can make the things counted without end, so each is told that often. */
static BOOL power_of_ten(uint32_t k) {
    while (k && k % 10 == 0) k /= 10; /* 0 once the count wraps: not a power of ten */
    return k == 1;
}

/* What Metal asks while it reads a serialized pipeline. */
@interface HvContext : NSObject {
@public
    __unsafe_unretained HvReplay *replay;
    uint32_t task;
    NSString *failure;
    id standIn; /* what a failed function lookup returns: nil, except for compute pipelines (makeSerialized) */
    NSMapTable *refs; /* each function it handed Metal: the guest's ref for it (note_building) */
}
@end

/* Bytes a pixel takes in the uncompressed formats a surface gets viewed as; 0 for one not listed. */
static NSUInteger pixel_bytes(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatA8Unorm: case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Unorm_sRGB: case MTLPixelFormatR8Snorm:
    case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: return 1;
    case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint: case MTLPixelFormatR16Float:
    case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Unorm_sRGB: case MTLPixelFormatRG8Snorm: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
    case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatA1BGR5Unorm: case MTLPixelFormatABGR4Unorm: case MTLPixelFormatBGR5A1Unorm: return 2;
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatR32Float: case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG16Snorm:
    case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRG16Float: case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatRGBA8Snorm: case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint: case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRG11B10Float: case MTLPixelFormatRGB9E5Float:
    case MTLPixelFormatBGR10A2Unorm: return 4;
    case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Snorm:
    case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA16Float: return 8;
    case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: case MTLPixelFormatRGBA32Float: return 16;
    default: return 0;
    }
}

/* Metal aborts the app, rather than failing, on a linear texture whose rows don't hold its width or don't fit the buffer.
   A view the guest makes in another format, or a surface read with the wrong description, can ask for one. */
static BOOL linear_texture_fits(id<MTLDevice> device, MTLTextureDescriptor *t, uint64_t offset, uint64_t row, NSUInteger length) {
    NSUInteger bpp = pixel_bytes(t.pixelFormat);
    if (!bpp) return YES;
    static NSUInteger cached_align;
    if (!cached_align) cached_align = [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm] ?: 64;
    NSUInteger align = cached_align;
    BOOL ok = row >= t.width * bpp && (!align || (row % align == 0 && offset % align == 0)) && offset + row * t.height <= length;
    if (!ok) {
        static int told;
        if (told++ < 8) fprintf(stderr, "hengevm: pvg-metal: a %lux%lu format %lu texture can't sit on rows of %llu bytes at %llu in %lu: left empty\n",
                                (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat, row, offset, (unsigned long)length);
    }
    return ok;
}

static double now_seconds(void) {
    return (double)clock_gettime_nsec_np(CLOCK_MONOTONIC) * 1e-9;
}

/* How long the worker waits for a command buffer before going without what it wanted from it. The GPU's own watchdog
   ends a hung buffer within seconds (it comes back failed, and gpuFailed: takes it from there), so these are only a
   backstop: a wait this long is a GPU that is sick, and the worker, which every frame of the Mac's goes through, must
   not wait on it for ever. HV_PVG_GPU_WAIT for a frame's or a video surface's buffer (the frame is skipped, the
   upload tried again with the next exec); HV_PVG_SNAP_GPU_WAIT for Suspend's reading back and Resume's putting back,
   which move more. */
#ifndef HV_PVG_GPU_WAIT
#define HV_PVG_GPU_WAIT (3 * NSEC_PER_SEC)
#endif
#ifndef HV_PVG_SNAP_GPU_WAIT
#define HV_PVG_SNAP_GPU_WAIT (10 * NSEC_PER_SEC)
#endif
#ifdef HV_PVG_TEST_HOOKS
/* The tests' fake command buffers: how a wait on one goes (NULL: as on a real one, by its status) */
static BOOL (*cb_wait_test)(id<MTLCommandBuffer> cb, uint64_t ns);
#endif

/* Waits for cb (committed to r's queue) to be done, well or not, for at most ns, or until *stop (if given) is set: YES
   if it is done. Metal has no waitUntilCompleted with a timeout, and a completion handler can't be added once a buffer
   is committed, so this looks at its status, napping between looks (20 µs at first, then up to 200 µs: a frame's wait
   costs a few dozen looks). A buffer never committed is never done. On the worker only.
   Once a wait is given up, and while that buffer is still not done, the next ones give up at once: the queue runs its
   buffers in order, so everything committed after it waits behind it too, and the worker would otherwise lose ns to
   each frame, each video upload, while the GPU is stuck. */
static BOOL cb_wait(HvReplay *r, id<MTLCommandBuffer> cb, uint64_t ns, _Atomic int *stop) {
    if (!cb) return YES;
#ifdef HV_PVG_TEST_HOOKS
    if (cb_wait_test) return cb_wait_test(cb, ns);
#endif
    if (r->stuck_cb) {
        if (r->stuck_cb.status >= MTLCommandBufferStatusCompleted) r->stuck_cb = nil;
        else if (cb.status < MTLCommandBufferStatusCompleted) return NO;
    }
    uint64_t until = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + ns;
    useconds_t nap = 20;
    while (cb.status < MTLCommandBufferStatusCompleted) {
        if (stop && atomic_load(stop)) return NO;
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= until) {
            r->stuck_cb = cb;
            return NO;
        }
        usleep(nap);
        if (nap < 200) nap *= 2;
    }
    return YES;
}

/* How the half-float display surface is shown. WindowServer writes DISPLAY-ENCODED values there (extended sRGB: the guest's gray 128 is
   0.502, measured with the colour probe), so mode 0 passes them through. Mode 1 treats them as LINEAR light and applies the sRGB encode,
   which is what this code did before (and was only right for a linear display profile such as ACES CG Linear: with every other profile
   it encodes twice and the screen looks pale). HV_PVG_HALF=linear selects mode 1 at start; with the colour probe on, a file named
   mode-linear or mode-encoded in its folder switches it while running. */
static _Atomic int hv_half_mode = -1;
static int hv_half_mode_get(void) {
    int m = atomic_load(&hv_half_mode);
    if (m < 0) {
        const char *e = getenv("HV_PVG_HALF");
        m = e && !strcmp(e, "linear") ? 1 : 0;
        atomic_store(&hv_half_mode, m);
    }
    return m;
}
static float hv_half_shown(float v, int mode) { /* what the screen shows for a half-float component */
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    return mode == 1 ? (v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f) : v;
}

#include "pvg_colorprobe.h" /* HV_COLOR_PROBE: what the display is told and shown, for chasing colour problems */

/* A contiguous view of a task's guest pages; holes are left as zero pages. */
/* A range of task memory at one address, page by page. Metal pins a buffer's
   pages when the buffer is made, so a mapping never changes under a buffer: when
   the guest's pages change there is a new mapping and a new buffer. (A GPU still
   writing to pages the guest has freed and used again would corrupt its memory.) */
/* The n 16 KB pages at srcs (NULL: none, left as the zeros vm_allocate made) over the region at at: one vm_remap for
   each run of them that is consecutive in the host's memory (the guest's RAM is one region, so consecutive guest pages
   are one run), not one each: a full-screen surface is about 1,400 pages, and a remap of each cost about 2 ms on the
   worker, every time one was mapped. A run that fails is left zeros, its srcs made NULL; how many failed is returned. */
#ifndef HV_VM_REMAP
#define HV_VM_REMAP vm_remap
#endif
static unsigned remap_runs(vm_address_t at, void **srcs, uint64_t n) {
    unsigned failed = 0;
    for (uint64_t i = 0, j; i < n; i = j) {
        for (j = i + 1; srcs[i] && j < n && srcs[j] == (uint8_t *)srcs[j - 1] + 0x4000; j++) {}
        if (!srcs[i]) continue;
        vm_address_t to = at + (i << 14);
        vm_prot_t cur, max;
        kern_return_t kr = HV_VM_REMAP(mach_task_self(), &to, (j - i) << 14, 0, VM_FLAGS_OVERWRITE, mach_task_self(),
                                       (vm_address_t)srcs[i], FALSE, &cur, &max, VM_INHERIT_NONE);
        if (kr == KERN_SUCCESS) continue;
        fprintf(stderr, "hengevm: pvg-metal: vm_remap of %llu pages failed: %d\n", (unsigned long long)(j - i), kr);
        memset(srcs + i, 0, (j - i) * sizeof *srcs);
        failed++;
    }
    return failed;
}

static BOOL map_pages(HvReplay *r, uint32_t task, HvObject *o) {
    /* The length is the guest's: a mapping only ever backs a buffer, so none is bigger than a buffer can be */
    if (!o->length || o->length > r->max_buffer) return NO;
    uint64_t n = (o->length + 0x3fff) >> 14;
    vm_address_t at = 0;
    uint32_t *pages = calloc(n, 4);
    void **srcs = calloc(n, sizeof(void *));
    if (!pages || !srcs || vm_allocate(mach_task_self(), &at, n << 14, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        free(pages), free(srcs);
        return NO;
    }
    /* pages stays the guest's own numbers, one outside RAM too (mapped as zeros): refresh_pages compares them with
       the guest's table, and a number changed to 0 here never matched it, so every exec remapped the object (F3) */
    hv_pvg_lite_task_pages(r->lite, task, o->vpage, n, pages);
    BOOL in_ram = NO;
    for (uint64_t i = 0; i < n; i++) srcs[i] = pages[i] ? hv_pvg_lite_page(r->lite, pages[i]) : NULL, in_ram |= srcs[i] != NULL;
    /* The guest's pages under the GPU: always in a save while mapped (core/ramfile.h). Pinned before they are remapped:
       a page the guest gives back (the balloon) meanwhile is then zeroed where it is, never swapped for new memory
       under the remap, the GPU keeping the old page (S3-1) */
    hv_ramfile_gpu_map((void *)at, srcs, n);
    if (remap_runs(at, srcs, n)) {
        hv_ramfile_gpu_unmap((void *)at); /* first: once deallocated, a new mapping may get this address */
        vm_deallocate(mach_task_self(), at, n << 14);
        free(pages), free(srcs);
        return NO;
    }
    free(srcs);
    free(o->pages);
    o->pages = pages, o->pages_in_ram = in_ram;
    o->mapped = (uint8_t *)at;
    return YES;
}

/* A buffer over the mapping, that frees it when the last user is done. */
static id<MTLBuffer> buffer_over_mapping(HvReplay *r, HvObject *o) {
    uint8_t *start = o->mapped;
    NSUInteger bytes = (NSUInteger)((o->length + 0x3fff) & ~0x3fffull);
    id<MTLBuffer> b = [r->device newBufferWithBytesNoCopy:start length:bytes options:MTLResourceStorageModeShared
                                              deallocator:^(void *at, NSUInteger size) {
                                                  hv_ramfile_gpu_unmap(at); /* first: the address may be reused */
                                                  vm_deallocate(mach_task_self(), (vm_address_t)at, size);
                                              }];
    if (b) o->mapping_in_buffer = YES;
    return b;
}

/* The guest's table for this range against what is mapped: a change means a new mapping. YES if o->mapped is the
   guest's pages as its table has them now. NO if o isn't mapped, or its pages moved and no new mapping could be made:
   then the old one is over pages the guest may have given to other memory, so o is gone (made anew on its next use,
   object:task:) and nothing is written through it or read from it (gfx-commands F5). */
static BOOL refresh_pages(HvReplay *r, uint32_t task, HvObject *o) {
    if (!o->mapped) return NO;
    uint64_t n = (o->length + 0x3fff) >> 14;
    uint32_t stack[256];
    uint32_t *now = n <= 256 ? stack : malloc(n * 4);
    hv_pvg_lite_task_pages(r->lite, task, o->vpage, n, now);
    BOOL same = memcmp(now, o->pages, n * 4) == 0;
    if (now != stack) free(now);
    if (same) return YES;
    uint8_t *old = o->mapped;
    BOOL old_in_buffer = o->mapping_in_buffer;
    if (!map_pages(r, task, o)) {
        o->gone = YES;
        if (r->remaps_failed++ < 8)
            [r complain:[NSString stringWithFormat:@"pages of an object (%llu bytes) moved and could not be mapped again: "
                                                   "not used until made anew", o->length]];
        return NO;
    }
    if (!old_in_buffer) {
        hv_ramfile_gpu_unmap(old); /* first: once deallocated, a new mapping may get this address */
        vm_deallocate(mach_task_self(), (vm_address_t)old, n << 14);
    }
    o->mapping_in_buffer = NO;
    o->generation++;
    o->guest_copy = nil;
    if (o->plain_buffer) o->object = buffer_over_mapping(r, o);
    r->remaps++;
    return YES;
}

/* Where the host writes n bytes into a guest buffer at off (an answer, a tile size): its pages as the guest's table has
   them now, for a write through a mapping of pages it has since moved would land in what is someone else's memory now
   (gfx-commands F5). NULL if o is not mapped or the bytes are not all within it. */
static uint8_t *guest_out(HvReplay *r, uint32_t task, HvObject *o, uint64_t off, uint64_t n) {
    if (!o || !refresh_pages(r, task, o)) return NULL;
    return off <= o->length && o->length - off >= n ? o->mapped + off : NULL;
}

static void set_framebuffer_only(MTLTextureDescriptor *t, BOOL on) {
    SEL sel = sel_registerName("setFramebufferOnly:");
    if ([t respondsToSelector:sel]) ((void (*)(id, SEL, BOOL))objc_msgSend)(t, sel, on);
}

/* What Apple's host takes from both serialized forms: flags (framebuffer only bit 4, GPU
   optimized bit 6) and resource options, of which iOS has no managed memory. */
static void descriptor_common(MTLTextureDescriptor *t, uint8_t flags, uint16_t options) {
    set_framebuffer_only(t, (flags >> 4) & 1);
    t.allowGPUOptimizedContents = (flags >> 6) & 1;
    MTLResourceOptions o = options & 0x3ff;
#if TARGET_OS_IPHONE
    if ((o & 0xf0) == 0x10) o = (o & ~(MTLResourceOptions)0xf0) | MTLResourceStorageModeShared;
#endif
    t.resourceOptions = o;
}

/* Metal aborts the whole app on a swizzle channel outside 0...5 (seen as 193 when a surface's description was
   read short, see makeMappedTexture), so an unknown one falls back to the identity, logged a few times. */
static MTLTextureSwizzleChannels safe_swizzle(const uint8_t *sw, const uint8_t *desc, size_t desc_len, const char *what) {
    if (!(sw[0] | sw[1] | sw[2] | sw[3])) /* all Zero: never meant, it would draw nothing; an unread field */
        return MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
    for (int i = 0; i < 4; i++) {
        if (sw[i] > MTLTextureSwizzleAlpha) {
            static int told;
            if (told++ < 3) {
                fprintf(stderr, "hengevm: pvg-metal: %s swizzle %u %u %u %u is not a Metal swizzle; using RGBA. desc:", what, sw[0], sw[1], sw[2], sw[3]);
                for (size_t j = 0; j < desc_len; j++) fprintf(stderr, " %02x", desc[j]);
                fputc('\n', stderr);
            }
            return MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
        }
    }
    return MTLTextureSwizzleChannelsMake(sw[0], sw[1], sw[2], sw[3]);
}

/* Metal's pixel format setter stops the app on a number it doesn't know: those are Invalid, which the checks refuse */
static MTLPixelFormat guest_format(uint32_t f) {
    return hv_pvg_format_known(f) ? (MTLPixelFormat)f : MTLPixelFormatInvalid;
}

/* The two serialized forms of a texture description. */
static MTLTextureDescriptor *descriptor_v1(const uint8_t *d) {
    MTLTextureDescriptor *t = [MTLTextureDescriptor new];
    t.textureType = (MTLTextureType)(d[0] & 0xf);
    t.usage = d[1];
    t.pixelFormat = guest_format(d[2] | d[3] << 8);
    t.width = *(const uint32_t *)(d + 4);
    t.height = *(const uint32_t *)(d + 8);
    t.depth = *(const uint32_t *)(d + 12);
    t.mipmapLevelCount = *(const uint16_t *)(d + 16);
    t.sampleCount = *(const uint16_t *)(d + 18);
    t.arrayLength = *(const uint16_t *)(d + 20);
    descriptor_common(t, d[0], *(const uint16_t *)(d + 22));
    return t;
}

static MTLTextureDescriptor *descriptor_v2(const uint8_t *d) {
    MTLTextureDescriptor *t = [MTLTextureDescriptor new];
    uint32_t usage, w, h, depth;
    uint16_t mips, samples, array, options;
    memcpy(&usage, d + 3, 4), memcpy(&w, d + 7, 4), memcpy(&h, d + 11, 4), memcpy(&depth, d + 15, 4);
    memcpy(&mips, d + 19, 2), memcpy(&samples, d + 21, 2), memcpy(&array, d + 23, 2), memcpy(&options, d + 25, 2);
    t.textureType = (MTLTextureType)(d[0] & 0xf);
    t.pixelFormat = guest_format(d[1] | d[2] << 8);
    t.usage = usage;
    t.width = w, t.height = h, t.depth = depth;
    t.mipmapLevelCount = mips, t.sampleCount = samples, t.arrayLength = array;
    descriptor_common(t, d[0], options);
    t.swizzle = safe_swizzle(d + 0x23, d, 0x27, "texture");
    if (t.usage & MTLTextureUsageRenderTarget) /* Metal aborts on a swizzled render target */
        t.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
    return t;
}

@implementation HvContext
- (void)resetFailureState { failure = nil; }
- (BOOL)hadLookupFailure { return failure != nil; }
- (NSString *)firstFailureReason { return failure; }
- (id)functionWithFunctionRef:(unsigned long long)ref {
    HvObject *o = ref <= UINT32_MAX ? [replay object:(uint32_t)ref task:task kind:KIND_FUNCTION] : nil;
    if (!o) {
        if (!failure) failure = [NSString stringWithFormat:@"function %llu", ref];
        return standIn;
    }
    if (o->object) {
        if (!refs) refs = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsObjectPointerPersonality valueOptions:NSPointerFunctionsStrongMemory];
        [refs setObject:@(ref) forKey:o->object];
    }
    return o->object;
}
@end

static BOOL snap_entry_is(HvReplay *r, uint32_t task, uint32_t ref, const uint32_t want[3]);

@implementation HvReplay

- (void)dealloc {
    for (uint32_t i = 0; i < exec_found_n; i++) exec_found[i] = nil;
    free((void *)exec_found);
    pass_desc = nil;
    present_then = nil;
}

- (void)complain:(NSString *)what {
    if ([complaints containsObject:what]) return;
    if (complaints.count >= 256) { /* each one is told once, but a guest can make new ones without end */
        if (!complaints_full) fprintf(stderr, "hengevm: pvg-metal: %lu problems told; no more from now on\n", (unsigned long)complaints.count);
        complaints_full = YES;
        return;
    }
    [complaints addObject:what];
    fprintf(stderr, "hengevm: pvg-metal: %s\n", what.UTF8String);
}

static HvTexDesc tex_desc(MTLTextureDescriptor *t) {
    return (HvTexDesc){(uint32_t)t.textureType, (uint32_t)t.pixelFormat, (uint32_t)t.width, (uint32_t)t.height, (uint32_t)t.depth,
                       (uint32_t)t.mipmapLevelCount, (uint32_t)t.sampleCount, (uint32_t)t.arrayLength, t.usage, t.resourceOptions};
}

/* A texture as made (a view: as its own type, format, levels and slices), for the checks of the commands that use it */
static HvTexDesc made_desc(id<MTLTexture> t) {
    uint64_t options = (uint64_t)t.cpuCacheMode | (uint64_t)t.storageMode << 4 | (uint64_t)t.hazardTrackingMode << 8;
    return (HvTexDesc){(uint32_t)t.textureType, (uint32_t)t.pixelFormat, (uint32_t)t.width, (uint32_t)t.height, (uint32_t)t.depth,
                       (uint32_t)t.mipmapLevelCount, (uint32_t)t.sampleCount, (uint32_t)t.arrayLength, t.usage, options};
}

/* Metal stops the whole app on a texture description it cannot make (see pvg_validate.h), so each is checked first:
   a bad one is told, and its object stays nil. len, offset and row: the buffer under a linear texture (len 0: none).
   Usage bits Metal doesn't take are dropped from t first, not refused: they are hints, and a texture refused for one
   is a texture missing (macOS's Liquid Glass blurs into textures with private bit 0x10000, and drew black without them). */
- (BOOL)refuse:(MTLTextureDescriptor *)t what:(NSString *)what buffer:(uint64_t)len offset:(uint64_t)offset row:(uint64_t)row {
    uint64_t extra = (uint64_t)t.usage & ~(uint64_t)HV_PVG_USAGE_OK;
    if (extra) {
        [self complain:[NSString stringWithFormat:@"%@: usage bits %llx dropped", what, extra]];
        t.usage = (MTLTextureUsage)((uint64_t)t.usage & HV_PVG_USAGE_OK);
    }
    HvTexDesc d = tex_desc(t);
    const char *why;
    if (len) {
        uint64_t align = hv_pvg_linear_bpp(d.format) ? [device minimumLinearTextureAlignmentForPixelFormat:t.pixelFormat] : 0;
        why = hv_pvg_linear_problem(&d, caps, len, offset, row, align);
    } else why = hv_pvg_texture_problem(&d, caps);
    if (!why) return NO;
    [self complain:[NSString stringWithFormat:@"%@: %s (type %u format %u %ux%ux%u, %u mips, %u samples, %u long, usage %llx, options %llx; offset %llu row %llu of %llu)",
                    what, why, d.type, d.format, d.width, d.height, d.depth, d.mips, d.samples, d.array, d.usage, d.options, offset, row, len]];
    return YES;
}

- (BOOL)refuse:(MTLTextureDescriptor *)t what:(NSString *)what {
    return [self refuse:t what:what buffer:0 offset:0 row:0];
}

/* A kernel that does nothing. Metal's deserializer aborts the whole app when a compute pipeline's function is nil, which
   happens when this OS cannot load the library macOS sent ("This library format is not supported on this platform", seen on
   iPadOS 17 with macOS 15). The lookup has failed either way, so makeSerialized drops the pipeline: a missing effect, not a
   crash. */
- (id)computeStandIn {
    if (!stand_in) {
        NSError *err = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:@"#include <metal_stdlib>\nkernel void hv_stand_in() {}\n" options:nil error:&err];
        stand_in = [lib newFunctionWithName:@"hv_stand_in"];
        if (!stand_in) [self complain:[NSString stringWithFormat:@"stand-in kernel: %@", err.localizedDescription]];
    }
    return stand_in;
}

/* A library this OS refused, tried once more relabelled for an older Metal (metallib.h: macOS 15's on iPadOS 17). */
- (id<MTLLibrary>)lowered:(NSData *)sent {
    const char *why = NULL;
    NSMutableData *copy = [sent mutableCopy];
    if (hv_metallib_relabel(copy.mutableBytes, copy.length, 2, &why) <= 0) return nil;
    dispatch_data_t data = dispatch_data_create(copy.bytes, copy.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *again = nil;
    id<MTLLibrary> library = [device newLibraryWithData:data error:&again];
    if (library) {
        atomic_fetch_add(&libs_lowered, 1);
        [self complain:[NSString stringWithFormat:@"library: relabelled for iOS 17%@", why ? [NSString stringWithFormat:@" (%s)", why] : @""]];
    } else {
        [self complain:[NSString stringWithFormat:@"library: relabelled for iOS 17, but still refused: %@", again.localizedDescription]];
    }
    return library;
}

/* A function's record in the guest's memory: {off, len} (its library's bytes), then the serializer's {tag, size}, the
   tag always this one. Apple's host (ParavirtualizedGraphicsGPUTask) reads it so, and refuses any other tag ("Invalid
   serializer command type"). */
#define HV_FUNCTION_RECORD_TAG 0xf

/* A function's library, as note_building names it: FNV-1a of the bytes the guest sent */
static char lib_hash_key;
static uint64_t hv_fnv1a64(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

- (id)makeFunction:(uint64_t)at task:(uint32_t)task {
    if (env.fail_libs) { /* testing: every library refused, as an OS that cannot load them does */
        [self complain:@"library: refused (HV_PVG_FAIL_LIBS)"];
        atomic_fetch_add(&libs_refused, 1);
        return nil;
    }
    struct __attribute__((packed)) { uint64_t off; uint32_t len; uint32_t tag, size; } h;
    if (hv_pvg_lite_task_read(lite, task, at, &h, sizeof h)) return nil;
    if (h.tag != HV_FUNCTION_RECORD_TAG) { /* {off, len} then {tag, size}: Apple's host takes only this tag */
        [self complain:@"function record of a tag other than 0xf refused"]; /* one message, whatever the tag */
        return nil;
    }
    if (!h.len || h.len > (64u << 20)) { /* the guest's length: no library is near this big */
        [self complain:[NSString stringWithFormat:@"library of %u bytes refused", h.len]];
        return nil;
    }
    NSMutableData *lib = [NSMutableData dataWithLength:h.len];
    if (!lib || hv_pvg_lite_task_read(lite, task, h.off, lib.mutableBytes, h.len)) return nil;
    dispatch_data_t data = dispatch_data_create(lib.bytes, lib.length, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *err = nil;
    id<MTLLibrary> library = [device newLibraryWithData:data error:&err];
    if (!library) library = [self lowered:lib];
    if (!library) {
        [self complain:[NSString stringWithFormat:@"library: %@", err.localizedDescription]];
        atomic_fetch_add(&libs_refused, 1);
        return nil;
    }
    {   /* m4 fork: while tracing, keep each library macOS sends, to read its shaders on a Mac */
        const char *tf = env.trace_file;
        if (tf && env.shader_dump) {
            NSString *dir = [[[NSString stringWithUTF8String:tf] stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"shaders"];
            [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
            NSString *name = [NSString stringWithFormat:@"%@-%llx.metallib", library.functionNames.firstObject ?: @"lib", (unsigned long long)at];
            [lib writeToFile:[dir stringByAppendingPathComponent:name] atomically:NO];
        }
    }
    /* The record names no function: its library holds the one it means, and only that. Apple's host takes a library
       of exactly one ("Unexpected function count in library") and asks for it by the one name there is. */
    if (library.functionNames.count != 1) { /* counted, not complained: each count would take one of complain's 256 */
        if (power_of_ten(++libs_many))
            fprintf(stderr, "hengevm: pvg-metal: library of %lu functions refused (one was expected); %u refused so far\n",
                    (unsigned long)library.functionNames.count, libs_many);
        return nil;
    }
    id function = [library newFunctionWithName:library.functionNames[0]];
    if (!function) atomic_fetch_add(&libs_refused, 1);
    else objc_setAssociatedObject(function, &lib_hash_key, @(hv_fnv1a64(lib.bytes, lib.length)), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return function;
}

/* A pipeline's shaders each in a slot of their own kind, before Metal builds it: the vertex function a vertex one, the
   fragment one a fragment one, a tile or compute pipeline's a kernel, and the helpers they link to visible or
   intersection functions. Nothing else checks this outside Metal's validation layer. This is a check of what the
   guest sent, not the fix for the iPad's two stops in the GPU driver on 9 Nov 2026 ("os_unfair_lock is corrupt" in
   createVertexProgramVariant, then a read of 0xe0 there). A shader in the wrong slot is one reading of those stops,
   not a proven one: no log shows it, and a build with this check (9a9baf8) ran about 809,000 draws without refusing a
   pipeline. Other readings fit as well: an object handed back for an entry that matched an old one, a fault in the
   driver itself, or memory corrupted elsewhere in this process. The guest's own Metal never puts a shader in another
   kind's slot (it validates the descriptor before sending it), so a pipeline that fails this was named wrong on this
   side; it is not built (each draw with it is skipped, as for any pipeline Metal refuses) and the complaint names the
   shader, its kind and the slot. The pipeline being built is noted for a crash in the driver (note_building). Returns
   NULL when all fit; that a vertex function is there at all is hv_pvg_pipeline_problem's. */
/* A deserialized pipeline description as hv_pvg_pipeline_problem (pvg_command.h) takes it */
static HvPipe pipe_desc(id descriptor, int form /* 0 render, 1 tile, 2 compute */) {
    HvPipe p = {.form = form};
    if (form == 2) {
        p.kernel = ((MTLComputePipelineDescriptor *)descriptor).computeFunction != nil;
        return p;
    }
    if (form == 1) {
        MTLTileRenderPipelineDescriptor *t = descriptor;
        p.kernel = t.tileFunction != nil, p.raster_samples = (uint32_t)t.rasterSampleCount;
        for (int i = 0; i < 8; i++) p.color[i] = (uint32_t)t.colorAttachments[i].pixelFormat;
        return p;
    }
    MTLRenderPipelineDescriptor *d = descriptor;
    p.vertex = d.vertexFunction != nil, p.fragment = d.fragmentFunction != nil;
    p.alpha_to_coverage = d.alphaToCoverageEnabled, p.alpha_to_one = d.alphaToOneEnabled;
    for (int i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        p.color[i] = (uint32_t)c.pixelFormat, p.blending[i] = c.blendingEnabled;
    }
    p.depth = (uint32_t)d.depthAttachmentPixelFormat, p.stencil = (uint32_t)d.stencilAttachmentPixelFormat;
    p.raster_samples = (uint32_t)d.rasterSampleCount, p.tess_factor = (uint32_t)MIN(d.maxTessellationFactor, (NSUInteger)UINT32_MAX);
    p.tess_mode = (uint32_t)d.tessellationPartitionMode, p.topology = (uint32_t)d.inputPrimitiveTopology;
    MTLVertexDescriptor *v = d.vertexDescriptor;
    for (NSUInteger i = 0; v && i < 31; i++) {
        MTLVertexAttributeDescriptor *a = v.attributes[i];
        if (a.format != MTLVertexFormatInvalid && a.bufferIndex + 1 > p.vertex_buffer) p.vertex_buffer = a.bufferIndex + 1;
    }
    return p;
}

static NSString *pipeline_stage_problem(id descriptor, int form /* 0 render, 1 tile, 2 compute */) {
    static const char *kinds[] = {"?", "vertex", "fragment", "kernel", "?", "visible", "intersection", "mesh", "object"};
    NSString *(^wrong)(id, const char *, MTLFunctionType) = ^NSString *(id f, const char *slot, MTLFunctionType want) {
        if (!f) return nil;
        if (![f respondsToSelector:@selector(functionType)]) return [NSString stringWithFormat:@"%s: not a function", slot];
        MTLFunctionType t = [f functionType];
        if (t == want) return nil;
        return [NSString stringWithFormat:@"%s function %@ is a %s function", slot, [f respondsToSelector:@selector(name)] ? [f name] : @"?",
                (NSUInteger)t < sizeof kinds / sizeof *kinds ? kinds[t] : "?"];
    };
    NSString *(^linked)(id, const char *) = ^NSString *(id l, const char *slot) {
        if (!l) return nil;
        NSMutableArray *all = [NSMutableArray array];
        for (NSString *key in @[@"functions", @"binaryFunctions", @"privateFunctions"])
            if ([l respondsToSelector:NSSelectorFromString(key)]) [all addObjectsFromArray:[l valueForKey:key] ?: @[]];
        if ([l respondsToSelector:@selector(groups)])
            for (NSArray *g in [[l valueForKey:@"groups"] allValues]) [all addObjectsFromArray:g];
        for (id f in all) {
            NSString *w = wrong(f, slot, MTLFunctionTypeVisible);
            if (w && [f respondsToSelector:@selector(functionType)] && [f functionType] == MTLFunctionTypeIntersection) w = nil;
            if (w) return w;
        }
        return nil;
    };
    NSString *w = nil;
#define HV_STAGE_SLOT(sel) ([descriptor respondsToSelector:@selector(sel)] ? [descriptor valueForKey:@#sel] : nil)
    if (form == 0) {
        if ((w = wrong(HV_STAGE_SLOT(vertexFunction), "vertex", MTLFunctionTypeVertex))) return w;
        if ((w = wrong(HV_STAGE_SLOT(fragmentFunction), "fragment", MTLFunctionTypeFragment))) return w;
        if ((w = linked(HV_STAGE_SLOT(vertexLinkedFunctions), "vertex helper"))) return w;
        return linked(HV_STAGE_SLOT(fragmentLinkedFunctions), "fragment helper");
    }
    if ((w = wrong(form == 1 ? HV_STAGE_SLOT(tileFunction) : HV_STAGE_SLOT(computeFunction), form == 1 ? "tile" : "compute",
                   MTLFunctionTypeKernel))) return w;
    return linked(HV_STAGE_SLOT(linkedFunctions), form == 1 ? "tile helper" : "compute helper");
#undef HV_STAGE_SLOT
}

static HvTargets pipeline_targets(id descriptor, BOOL tile) {
    HvTargets t = {{0}, 0, 0, 1};
    if (tile) {
        MTLTileRenderPipelineDescriptor *d = descriptor;
        for (int i = 0; i < 8; i++) t.color[i] = (uint32_t)d.colorAttachments[i].pixelFormat;
        t.samples = (uint32_t)d.rasterSampleCount;
    } else {
        MTLRenderPipelineDescriptor *d = descriptor;
        for (int i = 0; i < 8; i++) t.color[i] = (uint32_t)d.colorAttachments[i].pixelFormat;
        t.depth = (uint32_t)d.depthAttachmentPixelFormat, t.stencil = (uint32_t)d.stencilAttachmentPixelFormat;
        t.samples = (uint32_t)d.rasterSampleCount;
    }
    if (!t.samples) t.samples = 1;
    return t;
}

/* Why a pipeline can't draw in a pass: its colours (those it has) the pass's, its depth and stencil the same, and as
   many samples (a tile pipeline: its colours and samples) */
static const char *targets_problem(const HvTargets *pipe, const HvTargets *pass, BOOL tile) {
    for (int i = 0; i < 8; i++)
        if (pipe->color[i] && pipe->color[i] != pass->color[i]) return "a colour format";
    if (!tile && (pipe->depth != pass->depth || pipe->stencil != pass->stencil)) return "the depth or stencil format";
    if (pipe->samples != pass->samples) return "the sample count";
    return NULL;
}

/* The pipeline Metal is building, said if the process dies while it does (hv_pvg_metal_crash_note): each shader, its
   kind, the guest's ref for it and its library's hash, so a stop in the GPU driver names which function was in which
   role. Written before each build and dropped after; nothing reaches the log unless the build never returns. One
   buffer for the process, owned by the thread building; a second device's worker overwrites it only between builds of
   its own, which is when it is not read. */
static char building_text[1024];
static _Atomic uintptr_t building_thread;

static void note_building(id descriptor, int form /* 0 render, 1 tile, 2 compute */, uint32_t task, uint32_t ref, NSMapTable *refs) {
    static const char *forms[] = {"render", "tile", "compute"};
    static const char *kinds[] = {"?", "vertex", "fragment", "kernel", "?", "visible", "intersection", "mesh", "object"};
    char *b = building_text;
    size_t cap = sizeof building_text;
    __block size_t at = 0;
#define HV_PUT(...) do { if (at < cap) at += (size_t)snprintf(b + at, cap - at, __VA_ARGS__); } while (0)
    HV_PUT("hengevm: pvg: building a %s pipeline (task %u, ref %u) when the process died:", forms[form], task, ref);
    void (^one)(const char *, id) = ^(const char *slot, id f) {
        if (!f) { HV_PUT(" %s none;", slot); return; }
        MTLFunctionType t = [f respondsToSelector:@selector(functionType)] ? [f functionType] : (MTLFunctionType)0;
        NSString *name = [f respondsToSelector:@selector(name)] ? [f name] : nil;
        NSNumber *r = [refs objectForKey:f], *h = objc_getAssociatedObject(f, &lib_hash_key);
        HV_PUT(" %s '%s' (a %s function, ref %lld, library %016llx);", slot, name.UTF8String ?: "?",
               (NSUInteger)t < sizeof kinds / sizeof *kinds ? kinds[t] : "?", r ? r.longLongValue : -1LL,
               (unsigned long long)h.unsignedLongLongValue);
    };
    NSUInteger (^count)(id) = ^NSUInteger(id l) {
        NSUInteger n = 0;
        for (NSString *key in @[@"functions", @"binaryFunctions", @"privateFunctions"])
            if ([l respondsToSelector:NSSelectorFromString(key)]) n += [[l valueForKey:key] count];
        return n;
    };
    if (form == 0) {
        MTLRenderPipelineDescriptor *d = descriptor;
        one("vertex", d.vertexFunction), one("fragment", d.fragmentFunction);
        HV_PUT(" %lu vertex and %lu fragment helpers", (unsigned long)count(d.vertexLinkedFunctions), (unsigned long)count(d.fragmentLinkedFunctions));
    } else if (form == 1) {
        one("tile", ((MTLTileRenderPipelineDescriptor *)descriptor).tileFunction);
        HV_PUT(" %lu helpers", (unsigned long)count(((MTLTileRenderPipelineDescriptor *)descriptor).linkedFunctions));
    } else {
        one("compute", ((MTLComputePipelineDescriptor *)descriptor).computeFunction);
        HV_PUT(" %lu helpers", (unsigned long)count(((MTLComputePipelineDescriptor *)descriptor).linkedFunctions));
    }
#undef HV_PUT
    if (at >= cap) at = cap - 2;
    b[at] = '\n', b[at + 1] = 0;
    atomic_store(&building_thread, (uintptr_t)pthread_self());
}

static void built(void) { atomic_store(&building_thread, 0); }

/* From a fatal signal's handler (host/vm.c): async-signal-safe, a write(2) of what note_building wrote, when this
   thread was building a pipeline */
void hv_pvg_metal_crash_note(void) {
    if (atomic_load(&building_thread) != (uintptr_t)pthread_self()) return;
    ssize_t left = (ssize_t)strnlen(building_text, sizeof building_text);
    const char *p = building_text;
    while (left > 0) {
        ssize_t n = write(2, p, (size_t)left);
        if (n <= 0) break;
        p += n, left -= n;
    }
}

static void pipeline_timed(HvReplay *r, double t) {
    r->pipelines_built++, r->pipeline_time += t;
    if (t > r->pipeline_max) r->pipeline_max = t;
}

- (id)makeSerialized:(uint64_t)at length:(uint32_t)size task:(uint32_t)task kind:(HvKind *)kind into:(HvObject *)o {
    if (size < 8) { [self complain:[NSString stringWithFormat:@"serialized object of %u bytes", size]]; return nil; }
    NSMutableData *all = [NSMutableData dataWithLength:size];
    if (hv_pvg_lite_task_read(lite, task, at, all.mutableBytes, size)) return nil;
    const uint32_t *w = all.bytes;
    uint32_t tag = w[0];
    /* Each form's fields lie within what was read: {tag, size, ref, length} and the length's data for a pipeline, the
       bits and LOD clamps for a sampler, the flags and each side used for depth and stencil. */
    BOOL pipeline = tag == 0xe || tag == 0x33 || tag == 0xb;
    uint32_t need = pipeline ? 16 : tag == 3 ? 28 : tag == 4 ? 16 : 0;
    if (tag == 4 && size >= 16) need = (w[3] >> 5) & 1 ? 40 : (w[3] >> 4) & 1 ? 28 : 16;
    if (size < need || (pipeline && w[3] > size - 16)) {
        [self complain:[NSString stringWithFormat:@"serialized object tag %u: %u bytes, too few for its fields", tag, size]];
        return nil;
    }
    *kind = tag == 0xe ? KIND_RENDER_PIPELINE : tag == 0x33 ? KIND_TILE_PIPELINE : tag == 0xb ? KIND_COMPUTE_PIPELINE
          : tag == 3 ? KIND_SAMPLER : tag == 0xd ? KIND_FENCE : tag == 4 ? KIND_DEPTH_STENCIL : KIND_NONE;
    if (tag == 0xe || tag == 0x33) { /* render or tile pipeline: {tag, size, ref, length, data} */
        id<HvMetalSPI> spi = (id)device;
        HvContext *ctx = [HvContext new];
        ctx->replay = self, ctx->task = task;
        dispatch_data_t data = dispatch_data_create((const uint8_t *)all.bytes + 16, w[3], NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        id descriptor = tag == 0xe ? [spi newRenderPipelineDescriptorWithSerializedData:data deserializationContext:ctx]
                                   : [spi newTileRenderPipelineDescriptorWithSerializedData:data deserializationContext:ctx];
        if (!descriptor || ctx->failure) {
            [self complain:[NSString stringWithFormat:@"pipeline descriptor %u: %@", tag, ctx->failure]];
            return nil;
        }
        NSString *mixed = pipeline_stage_problem(descriptor, tag == 0xe ? 0 : 1);
        if (mixed) { /* a shader in another kind's slot would corrupt the driver's memory (pipeline_stage_problem) */
            [self complain:[NSString stringWithFormat:@"pipeline %u not built: %@", tag, mixed]];
            return nil;
        }
        HvPipe pd = pipe_desc(descriptor, tag == 0xe ? 0 : 1);
        const char *why = hv_pvg_pipeline_problem(&pd, caps);
        if (why) { /* Metal would stop the app on it */
            [self complain:[NSString stringWithFormat:@"pipeline %u not built: %s", tag, why]];
            return nil;
        }
        NSError *err = nil;
        note_building(descriptor, tag == 0xe ? 0 : 1, task, w[2], ctx->refs);
        double t0 = now_seconds();
        id state = tag == 0xe ? [device newRenderPipelineStateWithDescriptor:descriptor error:&err]
                              : [device newRenderPipelineStateWithTileDescriptor:descriptor options:MTLPipelineOptionNone reflection:nil error:&err];
        built();
        pipeline_timed(self, now_seconds() - t0);
        if (!state) [self complain:[NSString stringWithFormat:@"pipeline %u: %@", tag, err.localizedDescription]];
        if (!state && atomic_load(&libs_lowered)) atomic_fetch_add(&libs_refused, 1); /* relabelled code this OS can't build */
        o->targets = pipeline_targets(descriptor, tag == 0x33);
        o->patch_points = -1, o->tess_weight = 1, o->patch_rec = 0, o->patch_step = 0, o->patch_index = 0;
        if (tag == 0xe) {
            id<MTLFunction> vf = ((MTLRenderPipelineDescriptor *)descriptor).vertexFunction;
            if ([vf respondsToSelector:@selector(patchType)] && vf.patchType != MTLPatchTypeNone) {
                o->patch_points = (int32_t)vf.patchControlPointCount;
                NSUInteger tf = ((MTLRenderPipelineDescriptor *)descriptor).maxTessellationFactor;
                tf = tf < 1 ? 1 : tf > 64 ? 64 : tf;
                o->tess_weight = (uint32_t)(tf * tf);
                o->patch_rec = vf.patchType == MTLPatchTypeQuad ? 12 : 8;
                o->patch_step = (uint32_t)((MTLRenderPipelineDescriptor *)descriptor).tessellationFactorStepFunction;
                MTLTessellationControlPointIndexType ct = ((MTLRenderPipelineDescriptor *)descriptor).tessellationControlPointIndexType;
                o->patch_index = ct == MTLTessellationControlPointIndexTypeUInt16 ? 2 : ct == MTLTessellationControlPointIndexTypeUInt32 ? 4 : 0;
            }
        }
        if (tag == 0xe && env.shader_dump) { /* m4 fork: blend state as this iOS read it */
            MTLRenderPipelineDescriptor *rp = descriptor;
            for (int i = 0; i < 4; i++) {
                MTLRenderPipelineColorAttachmentDescriptor *c = rp.colorAttachments[i];
                if (c.pixelFormat == MTLPixelFormatInvalid) continue;
                fprintf(stderr, "hvpipe: %s c%d fmt %lu blend %d rgb %lu*src %lu %lu*dst alpha %lu*src %lu %lu*dst mask %lx\n",
                        rp.fragmentFunction.name.UTF8String ?: "?", i, (unsigned long)c.pixelFormat, (int)c.blendingEnabled,
                        (unsigned long)c.sourceRGBBlendFactor, (unsigned long)c.rgbBlendOperation, (unsigned long)c.destinationRGBBlendFactor,
                        (unsigned long)c.sourceAlphaBlendFactor, (unsigned long)c.alphaBlendOperation, (unsigned long)c.destinationAlphaBlendFactor,
                        (unsigned long)c.writeMask);
            }
        }
        if (state && tag == 0xe && hv_pvg_tracing()) { /* the name the trace gives it (trace:  pipeline) */
            MTLRenderPipelineDescriptor *rp = descriptor;
            objc_setAssociatedObject(state, "hvname", [NSString stringWithFormat:@"%@ / %@ -> fmt %lu blend %d", rp.vertexFunction.name, rp.fragmentFunction.name, (unsigned long)rp.colorAttachments[0].pixelFormat, (int)rp.colorAttachments[0].blendingEnabled], OBJC_ASSOCIATION_RETAIN);
        }
        return state;
    }
    if (tag == 0xb) { /* compute pipeline: {tag, size, ref, length, data} */
        id<HvMetalSPI> spi = (id)device;
        HvContext *ctx = [HvContext new];
        ctx->replay = self, ctx->task = task;
        dispatch_data_t data = dispatch_data_create((const uint8_t *)all.bytes + 16, w[3], NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        ctx->standIn = [self computeStandIn];
        id descriptor = [spi newComputePipelineDescriptorWithSerializedData:data deserializationContext:ctx];
        if (!descriptor || ctx->failure) {
            [self complain:[NSString stringWithFormat:@"compute descriptor: %@", ctx->failure]];
            return nil;
        }
        NSString *mixed = pipeline_stage_problem(descriptor, 2);
        if (mixed) {
            [self complain:[NSString stringWithFormat:@"compute pipeline not built: %@", mixed]];
            return nil;
        }
        HvPipe pd = pipe_desc(descriptor, 2);
        const char *why = hv_pvg_pipeline_problem(&pd, caps);
        if (why) {
            [self complain:[NSString stringWithFormat:@"compute pipeline not built: %s", why]];
            return nil;
        }
        NSError *err = nil;
        note_building(descriptor, 2, task, w[2], ctx->refs);
        double t0 = now_seconds();
        id state = [device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionNone reflection:nil error:&err];
        built();
        pipeline_timed(self, now_seconds() - t0);
        if (!state) [self complain:[NSString stringWithFormat:@"compute pipeline: %@", err.localizedDescription]];
        if (!state && atomic_load(&libs_lowered)) atomic_fetch_add(&libs_refused, 1);
        return state;
    }
    if (tag == 3) { /* sampler: {tag, size, ref, bits, lod min, lod max, ...} */
        uint64_t bits;
        memcpy(&bits, w + 3, 8);
        MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
        /* The fields are wider than Metal's choices, and Metal stops the app on one past them: each is kept within them */
        d.minFilter = (MTLSamplerMinMagFilter)MIN((uint32_t)(bits & 3), (uint32_t)MTLSamplerMinMagFilterLinear);
        d.magFilter = (MTLSamplerMinMagFilter)MIN((uint32_t)((bits >> 2) & 3), (uint32_t)MTLSamplerMinMagFilterLinear);
        d.lodAverage = (bits >> 4) & 1;
        d.compareFunction = (MTLCompareFunction)((bits >> 5) & 7);
        d.sAddressMode = (MTLSamplerAddressMode)MIN((uint32_t)((bits >> 8) & 15), (uint32_t)MTLSamplerAddressModeClampToBorderColor);
        d.tAddressMode = (MTLSamplerAddressMode)MIN((uint32_t)((bits >> 12) & 15), (uint32_t)MTLSamplerAddressModeClampToBorderColor);
        d.rAddressMode = (MTLSamplerAddressMode)MIN((uint32_t)((bits >> 16) & 15), (uint32_t)MTLSamplerAddressModeClampToBorderColor);
        d.borderColor = (MTLSamplerBorderColor)MIN((uint32_t)((bits >> 20) & 15), (uint32_t)MTLSamplerBorderColorOpaqueWhite);
        d.mipFilter = (MTLSamplerMipFilter)MIN((uint32_t)((bits >> 24) & 3), (uint32_t)MTLSamplerMipFilterLinear);
        NSUInteger aniso = (bits >> 26) & 31;
        d.maxAnisotropy = aniso ? MIN(aniso, (NSUInteger)16) : 1;
        d.normalizedCoordinates = (bits >> 31) & 1;
        d.supportArgumentBuffers = (bits >> 32) & 1;
        if (!d.normalizedCoordinates) { /* pixel coordinates: one filter, no mipmaps, no anisotropy, and only the modes that
                                           clamp (Metal: "non-normalized coords require minFilter==magFilter", ...) */
            d.magFilter = d.minFilter, d.mipFilter = MTLSamplerMipFilterNotMipmapped, d.maxAnisotropy = 1;
            MTLSamplerAddressMode m[3] = {d.sAddressMode, d.tAddressMode, d.rAddressMode};
            for (int i = 0; i < 3; i++)
                if (m[i] != MTLSamplerAddressModeClampToZero && m[i] != MTLSamplerAddressModeClampToBorderColor) m[i] = MTLSamplerAddressModeClampToEdge;
            d.sAddressMode = m[0], d.tAddressMode = m[1], d.rAddressMode = m[2];
        }
        float lod[2];
        memcpy(lod, w + 5, 8);
        d.lodMinClamp = lod[0], d.lodMaxClamp = lod[1];
        id s = [device newSamplerStateWithDescriptor:d];
        if (!s) [self complain:@"sampler failed"];
        return s;
    }
    if (tag == 0xd) return [device newFence];
    if (tag == 4) { /* depth and stencil: {tag, size, ref, flags, front {ops, read, write}, back {...}} */
        MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
        uint32_t f = w[3];
        d.depthCompareFunction = (MTLCompareFunction)(f & 7);
        d.depthWriteEnabled = (f >> 3) & 1;
        for (int side = 0; side < 2; side++) {
            if (!((f >> (4 + side)) & 1)) continue;
            const uint32_t *t = w + 4 + side * 3;
            MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
            sd.stencilCompareFunction = (MTLCompareFunction)(t[0] & 7);
            sd.stencilFailureOperation = (MTLStencilOperation)((t[0] >> 3) & 7);
            sd.depthFailureOperation = (MTLStencilOperation)((t[0] >> 6) & 7);
            sd.depthStencilPassOperation = (MTLStencilOperation)((t[0] >> 9) & 7);
            sd.readMask = t[1], sd.writeMask = t[2];
            if (side == 0) d.frontFaceStencil = sd;
            else d.backFaceStencil = sd;
        }
        return [device newDepthStencilStateWithDescriptor:d];
    }
    [self complain:[NSString stringWithFormat:@"serialized object tag %u", tag]];
    return nil;
}

/* A heap: {guest page, length, flags}. As on Apple's host its memory is the GPU's
   own, filled from the guest's pages when the guest says those changed. */
- (void)makeHeap:(HvObject *)o at:(uint64_t)at task:(uint32_t)task {
    struct __attribute__((packed)) { uint32_t vpage; uint64_t length; uint8_t flags; } h;
    if (hv_pvg_lite_task_read(lite, task, at, &h, sizeof h)) return;
    /* The length is the guest's: no heap past what a buffer over it can be, and all of them within the GPU's memory */
    uint64_t now = atomic_load(heap_total);
    if (!h.length || h.length > max_buffer || h.length > heap_limit || now > heap_limit - h.length) {
        [self complain:[NSString stringWithFormat:@"heap of %llu bytes refused (%llu MB of heaps already)", h.length, now >> 20]];
        return;
    }
    MTLHeapDescriptor *d = [MTLHeapDescriptor new];
    d.size = (NSUInteger)h.length;
    d.type = MTLHeapTypePlacement;
    d.storageMode = MTLStorageModePrivate;
    d.hazardTrackingMode = h.flags & 2 ? MTLHazardTrackingModeTracked : MTLHazardTrackingModeDefault;
    id<MTLHeap> heap = [device newHeapWithDescriptor:d];
    if (!heap && d.hazardTrackingMode == MTLHazardTrackingModeTracked) { /* not every GPU tracks placement heaps */
        d.hazardTrackingMode = MTLHazardTrackingModeDefault;
        heap = [device newHeapWithDescriptor:d];
        [self complain:@"placement heaps here cannot be tracked"];
    }
    if (!heap) { [self complain:[NSString stringWithFormat:@"heap of %llu bytes failed", h.length]]; return; }
    o->object = heap;
    MTLSizeAndAlign whole = [device heapBufferSizeAndAlignWithLength:(NSUInteger)h.length options:MTLResourceStorageModePrivate];
    if (whole.size <= heap.size) o->heap_whole = [heap newBufferWithLength:(NSUInteger)h.length options:MTLResourceStorageModePrivate offset:0];
    o->length = (h.length + 0x3fff) & ~0x3fffull;
    o->vpage = h.vpage;
    o->guest_backed = YES;
    o->heap_total = heap_total, o->heap_size = h.length;
    uint64_t all = atomic_fetch_add(heap_total, h.length) + h.length;
    if (env.heap_log) fprintf(stderr, "hengevm: pvg-metal: heap %llu KB at page %u, flags %x, now %llu MB of heaps\n", h.length >> 10, h.vpage, h.flags, all >> 20);
}

/* A buffer in a heap: {heap, offset, length}. */
- (void)makeHeapBuffer:(HvObject *)o at:(uint64_t)at task:(uint32_t)task {
    struct __attribute__((packed)) { uint32_t heap; uint64_t offset, length; } h;
    if (hv_pvg_lite_task_read(lite, task, at, &h, sizeof h)) return;
    HvObject *parent = [self object:h.heap task:task kind:KIND_HEAP];
    if (!parent) { [self complain:[NSString stringWithFormat:@"heap buffer of ref %u: no heap", h.heap]]; return; }
    id<MTLHeap> heap = parent->object;
    o->parent = parent;
    /* Within the heap, at its alignment: past it Metal stops the app */
    MTLSizeAndAlign sa = h.length && h.length <= heap.size ? [device heapBufferSizeAndAlignWithLength:(NSUInteger)h.length options:MTLResourceStorageModePrivate]
                                                         : (MTLSizeAndAlign){0, 0};
    if (sa.size && sa.align && h.offset % sa.align == 0 && sa.size <= heap.size && h.offset <= heap.size - sa.size)
        o->object = [heap newBufferWithLength:(NSUInteger)h.length options:MTLResourceStorageModePrivate offset:(NSUInteger)h.offset];
    /* None made: the guest's draws bind nothing there, and what it asks about it is answered 0 (info:). A buffer of
       its own instead, as before, cost up to the heap's size for each such entry, counted nowhere (gfx-commands F4). */
    if (!o->object) {
        heap_buffers_refused++;
        [self complain:[NSString stringWithFormat:@"heap buffer: %llu bytes at %llu of ref %u refused", h.length, h.offset, h.heap]];
    }
}

- (void)makeBuffer:(HvObject *)o at:(uint64_t)at task:(uint32_t)task {
    struct __attribute__((packed)) { uint64_t length; uint32_t vpage; uint32_t flags; } h;
    if (hv_pvg_lite_task_read(lite, task, at, &h, sizeof h)) return;
    o->length = (h.length + 0x3fff) & ~0x3fffull;
    o->vpage = h.vpage;
    if (!map_pages(self, task, o)) return;
    o->plain_buffer = YES;
    o->object = buffer_over_mapping(self, o);
    if (!o->object) [self complain:@"buffer failed"];
}

- (void)makeTexture:(HvObject *)o at:(uint64_t)at size:(uint32_t)size task:(uint32_t)task {
    if (size < 12 + 0x18) { [self complain:[NSString stringWithFormat:@"texture entry of %u bytes", size]]; return; }
    NSMutableData *all = [NSMutableData dataWithLength:size];
    if (hv_pvg_lite_task_read(lite, task, at, all.mutableBytes, size)) return;
    const uint8_t *d = all.bytes;
    /* {length, page}, a layout table, {tag, size, ref}, the description. */
    memcpy(&o->length, d, 8);
    memcpy(&o->vpage, d + 8, 4);
    const uint8_t *layout = d + 12;
    o->tex_mips = *(const uint16_t *)layout & 0x3fff;
    o->tex_slices = *(const uint16_t *)(layout + 2);
    memcpy(&o->tex_base, layout + 4, 8);
    memcpy(&o->tex_stride, layout + 12, 8);
    o->tex_block_h = layout[0x15];
    uint32_t table = o->tex_mips * 36;
    if (12 + 0x18 + table + 12 > size) { [self complain:@"texture entry too small"]; return; }
    const uint8_t *rec = layout + 0x18 + table;
    uint32_t tag = *(const uint32_t *)rec;
    BOOL v2 = tag == 0x34 || tag == 0x39;
    if (12 + 0x18 + table + 12 + (v2 ? 0x27 : 0x18) > size) { [self complain:@"texture entry cut short"]; return; }
    MTLTextureDescriptor *t = v2 ? descriptor_v2(rec + 12) : descriptor_v1(rec + 12);
    t.storageMode = MTLStorageModePrivate;
    t.usage |= MTLTextureUsageShaderRead;
    if ([self refuse:t what:@"texture"]) return;
    /* Its layout in the guest's memory, checked once here so the copies between the two (page:toGuest:) can trust it:
       every level and slice the texture has, each at its real size, within the memory. */
    HvTexDesc td = tex_desc(t);
    if (o->tex_mips && !hv_pvg_block(td.format).bytes) {
        [self complain:[NSString stringWithFormat:@"texture format %u: its guest memory is not kept up to date", td.format]];
        o->tex_mips = 0;
    } else if (o->tex_mips) {
        uint32_t slices = 0;
        const char *why = hv_pvg_layout_problem(&td, o->tex_mips, o->tex_slices, o->tex_base, o->tex_stride, o->tex_block_h, layout + 0x18, o->length, &slices);
        if (why) {
            [self complain:[NSString stringWithFormat:@"texture layout: %s (format %u %ux%ux%u type %u, %u levels %u slices, base %llu stride %llu, %llu bytes)",
                            why, td.format, td.width, td.height, td.depth, td.type, o->tex_mips, o->tex_slices, o->tex_base, o->tex_stride, o->length]];
            return;
        }
        o->tex_slices = slices;
        o->tex_layout = malloc(table);
        if (!o->tex_layout) return;
        memcpy(o->tex_layout, layout + 0x18, table);
    }
    o->object = [device newTextureWithDescriptor:t];
    if (o->object) o->guest_backed = YES;
    else [self complain:[NSString stringWithFormat:@"texture format %lu %lux%lu failed", (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height]];
}

/* A texture over a buffer: {tag 9, size, ref, buffer ref, offset, bytes per row, description}. */
- (void)makeLinearTexture:(HvObject *)o at:(uint64_t)at size:(uint32_t)size task:(uint32_t)task {
    struct __attribute__((packed)) { uint32_t tag, size, ref, buffer; uint64_t offset, row; uint8_t d[64]; } h;
    memset(&h, 0, sizeof h);
    if (hv_pvg_lite_task_read(lite, task, at, &h, MIN(size, (uint32_t)sizeof h))) return;
    if (h.tag == 0x34 || h.tag == 0x14 || h.tag == 1) { /* no guest memory at all: {tag, size, ref, description} */
        const uint8_t *desc = (const uint8_t *)&h.buffer;
        MTLTextureDescriptor *t = h.tag == 0x34 ? descriptor_v2(desc) : descriptor_v1(desc);
        t.storageMode = MTLStorageModePrivate;
        if ([self refuse:t what:@"texture"]) return;
        o->object = [device newTextureWithDescriptor:t];
        if (!o->object) [self complain:[NSString stringWithFormat:@"texture format %lu %lux%lu samples %lu failed", (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.sampleCount]];
        return;
    }
    if (h.tag == 0x15 || h.tag == 0x38) { /* in a heap: {tag, size, ref, heap, description, offset after 0x24 or 0x2c} */
        const uint8_t *desc = (const uint8_t *)&h.offset;
        uint64_t offset;
        memcpy(&offset, desc + (h.tag == 0x38 ? 0x2c : 0x24), 8);
        HvObject *parent = [self object:h.buffer task:task kind:KIND_HEAP];
        id<MTLHeap> heap = parent ? parent->object : nil;
        MTLTextureDescriptor *t = h.tag == 0x38 ? descriptor_v2(desc) : descriptor_v1(desc);
        t.resourceOptions = heap ? heap.resourceOptions : MTLResourceStorageModePrivate;
        if ([self refuse:t what:@"heap texture"]) return;
        o->parent = parent;
        /* Within the heap, at its alignment: past it Metal stops the app */
        MTLSizeAndAlign sa = heap ? [device heapTextureSizeAndAlignWithDescriptor:t] : (MTLSizeAndAlign){0, 0};
        if (sa.size && sa.align && offset % sa.align == 0 && sa.size <= heap.size && offset <= heap.size - sa.size)
            o->object = [heap newTextureWithDescriptor:t offset:(NSUInteger)offset];
        if (!o->object) {
            [self complain:[NSString stringWithFormat:@"heap texture format %lu %lux%lu at %llu of ref %u failed", (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, offset, h.buffer]];
            t.storageMode = MTLStorageModePrivate;
            if (![self refuse:t what:@"heap texture's stand-in"]) o->object = [device newTextureWithDescriptor:t];
        }
        return;
    }
    if (h.tag == 7 || h.tag == 8 || h.tag == 0x1b) { /* a view: {tag, size, ref, parent, format u16, type u16, levels, slices, swizzle} */
        struct __attribute__((packed)) { uint16_t format, type; uint64_t levels[2], slices[2]; uint8_t swizzle[4]; } v;
        memcpy(&v, &h.offset, sizeof v);
        HvObject *parent = [self viewParent:h.buffer task:task];
        id<MTLTexture> from = parent ? parent->object : nil;
        if (!from) { [self complain:[NSString stringWithFormat:@"view of nothing (tag %u, ref %u)", h.tag, h.buffer]]; return; }
        /* Metal stops the app on a view its parent cannot give: a format it can't be seen as, a type, levels or slices
           past it. Tag 7 keeps the parent's type, levels and slices. */
        HvTexDesc pd = made_desc(from);
        uint64_t all_slices = (uint64_t)pd.array * (pd.type == HV_TEX_CUBE || pd.type == HV_TEX_CUBE_ARRAY ? 6 : 1);
        const char *why = h.tag == 7 ? hv_pvg_view_problem(&pd, v.format, pd.type, 0, pd.mips, 0, all_slices)
                                     : hv_pvg_view_problem(&pd, v.format, v.type, v.levels[0], v.levels[1], v.slices[0], v.slices[1]);
        if (why) {
            [self complain:[NSString stringWithFormat:@"view tag %u: %s (format %u as %u, type %u as %u, levels %llu+%llu of %u, slices %llu+%llu of %llu)",
                            h.tag, why, pd.format, v.format, pd.type, v.type, v.levels[0], v.levels[1], pd.mips, v.slices[0], v.slices[1], all_slices]];
            return;
        }
        o->parent = parent;
        o->surface = parent->surface;
        over_surface(o);
        NSRange levels = NSMakeRange((NSUInteger)v.levels[0], (NSUInteger)v.levels[1]), slices = NSMakeRange((NSUInteger)v.slices[0], (NSUInteger)v.slices[1]);
        if (h.tag == 7) o->object = [from newTextureViewWithPixelFormat:(MTLPixelFormat)v.format];
        else if (h.tag == 8) o->object = [from newTextureViewWithPixelFormat:(MTLPixelFormat)v.format textureType:(MTLTextureType)v.type levels:levels slices:slices];
        else o->object = [from newTextureViewWithPixelFormat:(MTLPixelFormat)v.format textureType:(MTLTextureType)v.type levels:levels slices:slices
                                                     swizzle:safe_swizzle(v.swizzle, (const uint8_t *)&v, sizeof v, "view")];
        if (!o->object) [self complain:[NSString stringWithFormat:@"view tag %u of format %lu as %u failed", h.tag, (unsigned long)from.pixelFormat, v.format]];
        return;
    }
    if (h.tag != 9 && h.tag != 0x37) {
        uint32_t w[24] = {0};
        hv_pvg_lite_task_read(lite, task, at, w, sizeof w);
        NSMutableString *m = [NSMutableString stringWithFormat:@"serialized texture tag %u:", h.tag];
        for (int i = 0; i < 24; i++) [m appendFormat:@" %08x", w[i]];
        [self complain:m];
        return;
    }
    HvObject *b = [self object:h.buffer task:task kind:KIND_BUFFER];
    id<MTLBuffer> buffer = b ? b->object : nil;
    if (!buffer) { [self complain:@"linear texture without a buffer"]; return; }
    MTLTextureDescriptor *t = h.tag == 0x37 ? descriptor_v2(h.d) : descriptor_v1(h.d);
    t.resourceOptions = buffer.resourceOptions;
    set_framebuffer_only(t, NO);
    t.usage |= MTLTextureUsageShaderRead;
    if ([self refuse:t what:@"linear texture" buffer:buffer.length offset:h.offset row:h.row]) return;
    o->parent = b;
    o->follows = b, o->follows_generation = b->generation;
    if (linear_texture_fits(device, t, h.offset, h.row, buffer.length)) o->object = [buffer newTextureWithDescriptor:t offset:h.offset bytesPerRow:h.row];
    if (!o->object) [self complain:[NSString stringWithFormat:@"linear texture format %lu row %llu failed", (unsigned long)t.pixelFormat, h.row]];
}

/* A multi-plane surface's planes as the guest laid them out, in its descriptor from +0x80, 0x80 apart: offset +8,
   size +0x18, width +0x20, height +0x28, row +0x30, bytes per element +0x38. How many there are, or 0 if any doesn't
   lie within the guest's alloc bytes, or all of them packed are more than HV_PVG_VIDEO_MAX (what a completion
   compares and copies back, video_back). */
static uint32_t video_layout(const uint8_t *desc, uint64_t alloc, HvVideoPlane pl[4]) {
    uint32_t planes = desc[0x3e];
    if (planes < 2 || planes > 4) return 0;
    uint64_t at = 0;
    for (uint32_t i = 0; i < planes; i++) {
        uint64_t packed;
        if (!hv_pvg_video_plane(desc + 0x80 + i * 0x80, alloc, &pl[i]) || __builtin_mul_overflow(pl[i].row, pl[i].rows, &packed) ||
            __builtin_add_overflow(at, packed, &at) || at > HV_PVG_VIDEO_MAX)
            return 0;
    }
    return planes;
}

/* Where a surface's host side is found in planar: the guest memory (its page list, the first page's offset and alloc),
   the format and size, and the planes' layout. Every mapping with the same key reaches the same guest bytes through
   its own pages and has the same planes over them, so each copy between its pages and the shared IOSurface stays
   within both, as it does for the mapping that made it. Each page entry as what it reaches: a present page's address
   (bit 0 masked, as d3bb8ba did for the first), or none (0) for one not present, whatever its other bits: the same
   for every mapping that misses it. One page present and the other not are different memory (the zeros of the one
   without would go up over the other's pixels). */
static NSData *video_key(const uint32_t *list, uint64_t n, uint32_t first, uint64_t alloc, uint32_t fourcc, uint64_t w, uint64_t h,
                         const HvVideoPlane *pl, uint32_t planes) {
    size_t total = sizeof(uint64_t) * 6 + (size_t)planes * sizeof(uint64_t) * 6 + (size_t)n * sizeof(uint32_t);
    uint8_t stack_buf[1024];
    uint8_t *buf = total <= sizeof(stack_buf) ? stack_buf : malloc(total);
    if (!buf) return nil;
    uint8_t *dst = buf;
    const uint64_t head[6] = {first, alloc, fourcc, w, h, planes};
    memcpy(dst, head, sizeof head);
    dst += sizeof head;
    for (uint32_t i = 0; i < planes; i++) {
        const uint64_t p[6] = {pl[i].offset, pl[i].row, pl[i].rows, pl[i].row_bytes, pl[i].width, pl[i].bpe};
        memcpy(dst, p, sizeof p);
        dst += sizeof p;
    }
    uint32_t *pages_dst = (uint32_t *)dst;
    for (uint64_t i = 0; i < n; i++) {
        pages_dst[i] = list[i] & 1 ? list[i] & ~1u : 0;
    }
    NSData *k = [NSData dataWithBytes:buf length:total];
    if (buf != stack_buf) free(buf);
    return k;
}

/* macOS 13/14 compact plane layout: 0x40 bytes header, then 0x40 per plane */
static uint32_t video_layout_compact(const uint8_t *desc, uint64_t alloc, HvVideoPlane pl[4]) {
    uint32_t planes = 1;
    memcpy(&planes, desc + 0x24, 4);
    if (planes < 2 || planes > 4) return 0;
    uint64_t at = 0;
    for (uint32_t i = 0; i < planes; i++) {
        const uint8_t *cp = desc + 0x40 + i * 0x40;
        uint32_t v[6], base;
        memcpy(v, cp + 0x0c, sizeof v);
        memcpy(&base, cp + 0x08, 4);
        uint64_t off = v[0], size = v[1], w = v[2] >> 8, h = v[3] >> 8, row = v[4];
        uint16_t bpe = (uint16_t)v[5];
        if (base > off && base - off < size) size -= base - off, off = base;
        pl[i].offset = off;
        pl[i].row = row;
        pl[i].rows = (uint32_t)h;
        pl[i].width = (uint32_t)w;
        pl[i].bpe = bpe;
        pl[i].row_bytes = (uint64_t)w * bpe;
        uint64_t packed;
        if (__builtin_mul_overflow(pl[i].row, pl[i].rows, &packed) ||
            __builtin_add_overflow(at, packed, &at) || at > HV_PVG_VIDEO_MAX)
            return 0;
    }
    return planes;
}

/* A multi-plane surface as a real IOSurface, laid out as the guest laid it out (video_layout's planes). Its planes must
   hold the guest's rows, or there is none: copies between the two trust both. */
static IOSurfaceRef make_iosurface(const uint8_t *desc, uint64_t alloc, const HvVideoPlane *pl, uint32_t planes, BOOL compact) {
    uint32_t fourcc;
    uint64_t width, height;
    memcpy(&fourcc, desc + 4, 4);
    if (compact) {
        uint32_t w32, h32;
        memcpy(&w32, desc + 0x14, 4), memcpy(&h32, desc + 0x18, 4);
        width = w32 >> 8, height = h32 >> 8;
    } else {
        memcpy(&width, desc + 0x20, 8), memcpy(&height, desc + 0x28, 8);
    }
    NSMutableArray *info = [NSMutableArray new], *plain = [NSMutableArray new];
    uint64_t at = 0;
    for (uint32_t i = 0; i < planes; i++) {
        uint64_t size, packed = pl[i].row * pl[i].rows; /* video_layout's: no overflow */
        if (compact) {
            uint32_t v[6];
            memcpy(v, desc + 0x40 + i * 0x40 + 0x0c, sizeof v);
            size = v[1];
        } else {
            memcpy(&size, desc + 0x80 + i * 0x80 + 0x18, 8);
        }
        [info addObject:@{(__bridge id)kIOSurfacePlaneWidth: @(pl[i].width), (__bridge id)kIOSurfacePlaneHeight: @(pl[i].rows),
                          (__bridge id)kIOSurfacePlaneBytesPerRow: @(pl[i].row), (__bridge id)kIOSurfacePlaneOffset: @(pl[i].offset),
                          (__bridge id)kIOSurfacePlaneSize: @(size), (__bridge id)kIOSurfacePlaneBytesPerElement: @(pl[i].bpe),
                          (__bridge id)kIOSurfacePlaneElementWidth: @1, (__bridge id)kIOSurfacePlaneElementHeight: @1}];
        [plain addObject:@{(__bridge id)kIOSurfacePlaneWidth: @(pl[i].width), (__bridge id)kIOSurfacePlaneHeight: @(pl[i].rows),
                           (__bridge id)kIOSurfacePlaneBytesPerRow: @(pl[i].row), (__bridge id)kIOSurfacePlaneOffset: @(at),
                           (__bridge id)kIOSurfacePlaneSize: @(packed), (__bridge id)kIOSurfacePlaneBytesPerElement: @(pl[i].bpe),
                           (__bridge id)kIOSurfacePlaneElementWidth: @1, (__bridge id)kIOSurfacePlaneElementHeight: @1}];
        at += packed;
    }
    NSDictionary *props = @{(__bridge id)kIOSurfaceWidth: @(width), (__bridge id)kIOSurfaceHeight: @(height), (__bridge id)kIOSurfacePixelFormat: @(fourcc),
                            (__bridge id)kIOSurfaceAllocSize: @(alloc), (__bridge id)kIOSurfacePlaneInfo: info};
    IOSurfaceRef made = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    if (!made) {
        /* Video formats can refuse the guest's padding: request packed planes instead. */
        NSDictionary *plain_props = @{(__bridge id)kIOSurfaceWidth: @(width), (__bridge id)kIOSurfaceHeight: @(height), (__bridge id)kIOSurfacePixelFormat: @(fourcc), (__bridge id)kIOSurfacePlaneInfo: plain};
        made = IOSurfaceCreate((__bridge CFDictionaryRef)plain_props);
    }
    if (!made) return NULL;
    BOOL fits = IOSurfaceGetPlaneCount(made) >= planes;
    for (uint32_t i = 0; fits && i < planes; i++)
        fits = IOSurfaceGetHeightOfPlane(made, i) >= pl[i].rows && IOSurfaceGetWidthOfPlane(made, i) >= pl[i].width &&
               IOSurfaceGetBytesPerRowOfPlane(made, i) >= pl[i].row_bytes;
    if (!fits) {
        CFRelease(made);
        return NULL;
    }
    static int logged;
    if (logged++ < 4)
        for (uint32_t i = 0; i < planes; i++)
            fprintf(stderr, "hengevm: video plane %u guest off %llu row %llu h %llu; host row %zu h %zu\n",
                    i, (unsigned long long)pl[i].offset, (unsigned long long)pl[i].row, (unsigned long long)pl[i].rows,
                    IOSurfaceGetBytesPerRowOfPlane(made, i), IOSurfaceGetHeightOfPlane(made, i));
    return made;
}

/* Has the guest changed a plane's row since its bytes last went up (the shadow)? Under video_lock. The bytes it read
   of each, the shadow's and the guest's, added to *bytes. */
static BOOL video_changed(HvVideo *v, const uint8_t *from, uint64_t *bytes) {
    for (uint32_t i = 0; i < v->plane_count; i++) {
        HvVideoPlane p = v->plane[i];
        for (uint64_t y = 0; y < p.rows; y++) {
            *bytes += p.row_bytes;
            if (memcmp(v->shadow + p.offset + y * p.row, from + p.offset + y * p.row, (size_t)p.row_bytes)) return YES;
        }
    }
    return NO;
}

/* HV_PVG_HOST_DUMP=prefix (debugging): the host surface as it is just before the guest's pixels go over it, for the
   first few multi-plane surfaces, with its layout in a .txt beside it: did the GPU draw something here that this copy
   throws away? With the IOSurface locked. */
static void host_dump(HvReplay *r, HvVideo *v) {
    const char *prefix = r->env.host_dump;
    static int host_dumped;
    if (!prefix || host_dumped >= 12) return;
    char path[1100];
    snprintf(path, sizeof path, "%s-%d.bin", prefix, host_dumped);
    FILE *f = fopen(path, "wb");
    if (f) fwrite(IOSurfaceGetBaseAddress(v->iosurface), 1, IOSurfaceGetAllocSize(v->iosurface), f), fclose(f);
    snprintf(path, sizeof path, "%s-%d.txt", prefix, host_dumped++);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "fourcc %08x w %zu h %zu alloc %zu guest %llu\n", IOSurfaceGetPixelFormat(v->iosurface), IOSurfaceGetWidth(v->iosurface),
                IOSurfaceGetHeight(v->iosurface), IOSurfaceGetAllocSize(v->iosurface), (unsigned long long)v->alloc);
        for (size_t i = 0; i < IOSurfaceGetPlaneCount(v->iosurface); i++)
            fprintf(f, "plane %zu off %ld row %zu h %zu w %zu bpe %zu\n", i,
                    (long)((uint8_t *)IOSurfaceGetBaseAddressOfPlane(v->iosurface, i) - (uint8_t *)IOSurfaceGetBaseAddress(v->iosurface)),
                    IOSurfaceGetBytesPerRowOfPlane(v->iosurface, i), IOSurfaceGetHeightOfPlane(v->iosurface, i),
                    IOSurfaceGetWidthOfPlane(v->iosurface, i), IOSurfaceGetBytesPerElementOfPlane(v->iosurface, i));
        fclose(f);
    }
}

/* The guest said it wrote a video surface (an exec's flag 1, run or dropped while the GPU is stopped): its copy is the
   newer, so sync_iosurface sends its changed rows up whole again, over what the GPU drew, and the rows whose changed
   bytes went onto the drawing (merged) */
static void video_guest_wrote(HvVideo *v) { v->host_newer = NO; }

/* The guest's pixels, through this mapping of them, into its IOSurface: the rows it changed since they last went up,
   exact, against a shadow of what went up (never against the IOSurface, which the GPU may have drawn into since).
   While the host's copy is the newer (an exec drew into it, not all of that is back in the guest's pages, and the
   guest hasn't said it wrote it since), only the bytes the guest changed go up, onto the GPU's drawing: the rest of
   each row is the guest's stale copy, which would put back what was there before. Before 10 Nov 2026 nothing went up
   then, until a flag 1 or all the drawing was back, which the guest may never ask for: what it wrote after the GPU
   drew (with only op 0x34's word, or none) stayed hidden (gate S3). GPU work still reading the surface, through any
   mapping of it, is let finish first.
   Compared at every use, whether or not the guest said it wrote it: macOS doesn't always say (an exec's flag 1, op
   0x34). On the iPad, 10 Nov 2026, 55 surfaces of a YouTube video in Edge changed with no word, and while only the
   word (or a check once a second) sent rows up, each such frame showed the surface's old picture first: the video
   jumped back, then forward. A 4K frame's compare is about 1 ms. */
static void sync_iosurface(HvReplay *r, HvObject *s) {
    HvVideo *v = s->video;
    if (!v) return;
    BOOL drawn = NO; /* the IOSurface holds drawing the guest's pages and the shadow don't */
    /* Some of the guest's bytes went onto drawing not yet back (the merged rows), and since then the guest said it wrote
       it (flag 1), or all the drawing counts as back: those rows go up whole */
    BOOL rewrite = NO;
    if (v->host_newer || v->merged_any) {
        os_unfair_lock_lock(&v->video_lock);
        BOOL back = v->back_gen == v->write_gen; /* all the GPU drew is in the guest's pages and the shadow, or never will be */
        os_unfair_lock_unlock(&v->video_lock);
        /* What differs from the shadow is the guest's. Back need not mean copied back: work that failed or never ran
           (backs_none), or a mapping dropped (backs_off), counts too, and leaves the GPU's bytes in the merged rows,
           where only the rewrite puts the guest's. After a real copy-back they already agree, and nothing is copied. */
        if (back) v->host_newer = NO, rewrite = v->merged_any;
        else if (v->host_newer) drawn = YES;
        else rewrite = YES;
    }
    const uint8_t *from = s->mapped + s->first;
    double t0 = now_seconds();
    os_unfair_lock_lock(&v->video_lock);
    BOOL changed = video_changed(v, from, &r->video_bytes_compared);
    os_unfair_lock_unlock(&v->video_lock);
    r->video_scans++, r->video_scan_time += now_seconds() - t0;
    if (!changed && !rewrite) return;
    /* Only the worker (here and in commit:) sets last_reader, so none newer comes before the upload is done; Metal's
       completion only lets it go */
    os_unfair_lock_lock(&v->reader_lock);
    id<MTLCommandBuffer> reader = v->last_reader;
    os_unfair_lock_unlock(&v->reader_lock);
    if (reader && reader.status < MTLCommandBufferStatusCompleted) { /* waited for holding neither lock: its completion takes reader_lock */
        double w0 = now_seconds();
        BOOL done = cb_wait(r, reader, HV_PVG_GPU_WAIT, NULL);
        r->video_waits++, r->video_wait_time += now_seconds() - w0;
        if (!done) { /* the GPU still reads it: not written under it. The shadow is as it was, so the next exec tries again */
            atomic_fetch_add(&r->gpu_waits_given_up, 1);
            [r complain:@"a video surface's reader still running after its longest wait: its update put off"];
            return;
        }
    }
    double u0 = now_seconds();
    os_unfair_lock_lock(&v->reader_lock);
    if (v->last_reader == reader) v->last_reader = nil; /* done */
    os_unfair_lock_unlock(&v->reader_lock);
    os_unfair_lock_lock(&v->video_lock);
    if (IOSurfaceLock(v->iosurface, 0, NULL) == KERN_SUCCESS) {
        host_dump(r, v);
        uint8_t *merged = v->merged; /* each plane's bits in turn */
        uint64_t rows = 0;
        for (uint32_t i = 0; i < v->plane_count; i++) {
            uint8_t *dst = IOSurfaceGetBaseAddressOfPlane(v->iosurface, i);
            size_t dst_row = IOSurfaceGetBytesPerRowOfPlane(v->iosurface, i);
            if (dst && drawn) {
                uint64_t n = hv_pvg_video_plane_merge(dst, dst_row, v->shadow, from, v->plane[i], merged);
                rows += n, v->merged_any |= n != 0;
            } else if (dst) {
                rows += hv_pvg_video_plane_update(dst, dst_row, v->shadow, from, v->plane[i], NULL, NULL);
                if (rewrite) rows += hv_pvg_video_plane_rewrite(dst, dst_row, v->shadow, merged, v->plane[i]);
            }
            r->video_bytes_compared += v->plane[i].rows * v->plane[i].row_bytes; /* each row compared to find the changed */
            merged += (v->plane[i].rows + 7) / 8;
        }
        if (rewrite) v->merged_any = NO;
        IOSurfaceUnlock(v->iosurface, 0, NULL);
        /* An upload is one that wrote a row: a rewrite after a real copy-back finds its rows already the guest's, and
           counting it would add to video_unsaid what macOS never wrote */
        r->video_rows += rows;
        if (rows) r->video_uploads++, r->video_merges += drawn;
    }
    os_unfair_lock_unlock(&v->video_lock);
    r->video_upload_time += now_seconds() - u0;
    const char *dump = r->env.surface_dump;
    static int dumped;
    if (dump && dumped < 6) {
        char path[1100];
        snprintf(path, sizeof path, "%s-%d.bin", dump, dumped++);
        FILE *f = fopen(path, "wb");
        if (f) fwrite(from, 1, v->alloc, f), fclose(f);
    }
}

/* What the GPU drew into a video surface, back into the guest's pages through this mapping of them (and the shadow),
   as an exec's sync_after does for other objects: on the GPU's completion of the work, before its stamp is given back
   (the guest reads it once it has the stamp). Only the rows the GPU changed, which keeps the guest's own writing since
   (hv_pvg_video_plane_back), and at most HV_PVG_VIDEO_MAX bytes compared (video_layout): once for each surface,
   whatever mappings of it the work used (back_once). All of an exec's, however many: the guest reads them once it
   has the stamp, so none is put off past it (gfx-render 2). gen: the surface's write_gen when the exec went in; once
   the newest is back, the host's copy is no longer the newer (sync_iosurface). s is kept until this is done, and with
   it the pages it writes. */
static void video_back(HvReplay *r, HvObject *s, uint64_t gen) {
    HvVideo *v = s->video;
    uint64_t t0 = stage_now(); /* nanoseconds: now_seconds' clock moves in whole microseconds, longer than a small one takes */
    os_unfair_lock_lock(&v->video_lock);
    if (atomic_load(&s->backs_off)) { /* dropped, its pages perhaps reused: not written, and no longer to come (backs_none) */
        if (gen > v->back_gen) v->back_gen = gen;
        os_unfair_lock_unlock(&v->video_lock);
        return;
    }
    if (IOSurfaceLock(v->iosurface, kIOSurfaceLockReadOnly, NULL) == KERN_SUCCESS) {
        uint8_t *guest = s->mapped + s->first;
        uint64_t rows = 0;
        BOOL all = YES;
        for (uint32_t i = 0; i < v->plane_count; i++) {
            const uint8_t *src = IOSurfaceGetBaseAddressOfPlane(v->iosurface, i);
            if (src) rows += hv_pvg_video_plane_back(guest, v->shadow, src, IOSurfaceGetBytesPerRowOfPlane(v->iosurface, i), v->plane[i]);
            else all = NO;
        }
        IOSurfaceUnlock(v->iosurface, kIOSurfaceLockReadOnly, NULL);
        if (all && gen > v->back_gen) v->back_gen = gen;
        r->video_backs++, r->video_back_rows += rows;
    }
    os_unfair_lock_unlock(&v->video_lock);
    r->video_back_time += (double)(stage_now() - t0) * 1e-9;
}

/* planar: a new host side under its key, and the keys of those gone since the last time let go (a weak table keeps
   them until then) */
static void planar_add(HvReplay *r, NSData *key, HvVideo *v) {
    if (r->planar.count >= r->planar_sweep) {
        NSMutableArray<NSData *> *gone = [NSMutableArray new];
        for (NSData *k in r->planar.keyEnumerator)
            if (![r->planar objectForKey:k]) [gone addObject:k];
        for (NSData *k in gone) [r->planar removeObjectForKey:k];
        r->planar_sweep = MAX((NSUInteger)64, r->planar.count * 2);
    }
    [r->planar setObject:v forKey:key];
}

/* A video surface whose mapping went: held (planar_kept, newest last) while it is among the most recent, within
   HV_PVG_PLANAR_KEPT bytes and HV_PVG_PLANAR_KEPT_MAX of them; the oldest let go first. One still mapped elsewhere
   only counts here: letting it go frees nothing, nor does it leave planar while its mappings have it. */
static void planar_unkeep(HvReplay *r, HvVideo *v) {
    NSUInteger at = [r->planar_kept indexOfObjectIdenticalTo:v];
    if (at == NSNotFound) return;
    [r->planar_kept removeObjectAtIndex:at];
    r->planar_kept_bytes -= v->held;
}
static void planar_keep(HvReplay *r, HvVideo *v) {
    planar_unkeep(r, v);
    [r->planar_kept addObject:v];
    r->planar_kept_bytes += v->held;
    while (r->planar_kept.count && (r->planar_kept_bytes > HV_PVG_PLANAR_KEPT || r->planar_kept.count > HV_PVG_PLANAR_KEPT_MAX)) {
        r->planar_kept_bytes -= r->planar_kept[0]->held;
        [r->planar_kept removeObjectAtIndex:0];
    }
}

/* The worker, before an exec draws into v: every copy-back of it still to come from a committed exec's completion
   (backs_due) made first, so none copies it back while this one draws into it. The GPU runs the queue in order, so the
   newest of them (back_cb) done means all are; their parts on complete_q follow at once. Bounded as cb_wait: NO if the
   GPU still had them at the end (it hangs, and the breaker stops what follows). */
static BOOL backs_wait(HvReplay *r, HvVideo *v) {
    if (!atomic_load(&v->backs_due)) return YES;
    os_unfair_lock_lock(&v->video_lock);
    id<MTLCommandBuffer> cb = v->back_cb;
    os_unfair_lock_unlock(&v->video_lock);
    if (!cb_wait(r, cb, HV_PVG_GPU_WAIT, NULL)) return NO;
    uint64_t until = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + HV_PVG_GPU_WAIT;
    for (useconds_t nap = 20; atomic_load(&v->backs_due); nap = nap < 200 ? nap * 2 : nap) {
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= until) return NO;
        usleep(nap);
    }
    return YES;
}
/* Copy-backs a dropped or failed exec was to make: none comes, so the surface is no longer behind for them (else every
   later exec waits for complete_q, and the guest's own writing never goes up: sync_iosurface). What it may have half
   drawn stays the host's; the guest's next write (flag 1) goes up over it as ever. */
static void backs_none(NSArray<HvObject *> *back, NSArray<NSNumber *> *gens) {
    for (NSUInteger i = 0; i < back.count; i++) {
        HvVideo *v = back[i]->video;
        uint64_t gen = gens[i].unsignedLongLongValue;
        os_unfair_lock_lock(&v->video_lock);
        if (gen > v->back_gen) v->back_gen = gen;
        os_unfair_lock_unlock(&v->video_lock);
    }
}

/* A video surface to go back on the next commit's completion (writeback), once whatever mappings of it ask */
static void back_once(NSMutableArray<HvObject *> *writeback, HvObject *s) {
    for (HvObject *w in writeback)
        if (w->video == s->video) return;
    [writeback addObject:s];
}

/* A guest surface (IOSurface) from its description: its pages in one host buffer (or a real IOSurface for planes). */
- (HvObject *)newSurface:(uint64_t)mapping {
    NSNumber *key = @(mapping);
    HvObject *surface = nil;
    const uint8_t *guest_desc = hv_pvg_lite_surface(lite, mapping);
    if (!guest_desc) { [self complain:[NSString stringWithFormat:@"surface %llu has no descriptor", mapping]]; return nil; }
    uint8_t desc[0x400];
    memcpy(desc, guest_desc, sizeof desc);
    uint32_t list, first;
    uint64_t alloc, row;
    /* macOS 15 and later: u64 allocation, width, height and row at 0x18..0x30, plane count at 0x3e, first byte at 0x60.
       macOS 14's (and Ventura's) driver writes a compact one: u32 allocation at 0x10, width and height << 8 at 0x14 and 0x18, u32 row
       at 0x1c, bytes per pixel at 0x20; plane count at 0x24. */
    BOOL compact = hv_pvg_lite_old_driver(lite) != 0;
    uint32_t compact_planes = 1;
    if (compact) memcpy(&compact_planes, desc + 0x24, 4);
    uint8_t planes = compact ? (uint8_t)MIN(MAX(compact_planes, 1u), 4u) : desc[0x3e];
    memcpy(&list, desc, 4), memcpy(&alloc, desc + 0x18, 8), memcpy(&row, desc + 0x30, 8), memcpy(&first, desc + 0x60, 4);
    if (compact) {
        uint32_t a32, r32;
        memcpy(&a32, desc + 0x10, 4), memcpy(&r32, desc + 0x1c, 4);
        alloc = a32, row = r32, first = 0;
    }
    if (!alloc || alloc > max_buffer || first > max_buffer - alloc) {
        [self complain:[NSString stringWithFormat:@"surface %llu of %llu bytes from %u refused", mapping, alloc, first]];
        return nil;
    }
    surface = [HvObject new];
    if (compact) memcpy(surface->described, desc, sizeof surface->described);
    surface->length = (alloc + first + 0x3fff) & ~0x3fffull;
    surface->row = row;
    surface->first = first;
    uint64_t sw, sh;
    memcpy(&sw, desc + 0x20, 8), memcpy(&sh, desc + 0x28, 8), memcpy(&surface->fourcc, desc + 4, 4);
    if (compact) {
        uint32_t w32, h32;
        memcpy(&w32, desc + 0x14, 4), memcpy(&h32, desc + 0x18, 4);
        sw = w32 >> 8, sh = h32 >> 8;
    }
    surface->surface_w = (uint32_t)sw, surface->surface_h = (uint32_t)sh;
    uint64_t n = surface->length >> 14;
    vm_address_t region = 0;
    const uint32_t *guest_pages = (list & 1) ? hv_pvg_lite_phys(lite, (uint64_t)(list & ~1u) << 12, n * 4) : NULL;
    uint32_t *pages = guest_pages ? malloc(n * 4) : NULL;
    void **srcs = pages ? calloc(n, sizeof(void *)) : NULL;
    if (!srcs || vm_allocate(mach_task_self(), &region, surface->length, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
        free(pages), free(srcs);
        [self complain:[NSString stringWithFormat:@"surface %llu has no pages", mapping]];
        return nil;
    }
    memcpy(pages, guest_pages, n * 4);
    surface->mapped = (uint8_t *)region;
    uint64_t misaligned = 0;
    for (uint64_t i = 0; i < n; i++) {
        void *src = (pages[i] & 1) ? hv_pvg_lite_phys(lite, (uint64_t)(pages[i] & ~1u) << 12, 0x4000) : NULL;
        if ((uintptr_t)src & 0x3fff) src = NULL, misaligned++;
        srcs[i] = src;
    }
    if (misaligned) {
        pages_misaligned += misaligned;
        [self complain:[NSString stringWithFormat:@"surface pages not on a 16 KB boundary: shown as zeros (fourcc %08x)", surface->fourcc]];
    }
    hv_ramfile_gpu_map((void *)region, srcs, n);
    if (remap_runs(region, srcs, n)) {
        hv_ramfile_gpu_relist((void *)region, srcs, n);
    }
    free(srcs);
    surface->object = [device newBufferWithBytesNoCopy:surface->mapped length:surface->length options:MTLResourceStorageModeShared
                                           deallocator:^(void *at, NSUInteger size) {
                                               hv_ramfile_gpu_unmap(at);
                                               vm_deallocate(mach_task_self(), (vm_address_t)at, size);
                                           }];
    surface->mapping_in_buffer = surface->object != nil;
    surfaces[key] = surface;
    if (planes >= 2) {
        HvVideoPlane pl[4];
        uint32_t pl_count = compact ? video_layout_compact(desc, alloc, pl) : video_layout(desc, alloc, pl);
        NSData *vk = pl_count ? video_key(pages, n, first, alloc, surface->fourcc, sw, sh, pl, pl_count) : nil;
        HvVideo *v = vk ? [planar objectForKey:vk] : nil;
        if (v) {
            planar_unkeep(self, v);
            static int told;
            if (told++ < 8) fprintf(stderr, "hengevm: pvg-metal: surface %llu shares the host surface of the same guest memory (fourcc %08x, %u planes)\n", mapping, surface->fourcc, pl_count);
        } else if (pl_count) {
            IOSurfaceRef made = make_iosurface(desc, alloc, pl, pl_count, compact);
            uint8_t *shadow = made ? calloc(1, (size_t)alloc) : NULL;
            uint8_t *merged = shadow ? calloc(1, (size_t)hv_pvg_video_row_bits(pl, pl_count)) : NULL;
            if (made && shadow && merged) {
                v = [HvVideo new];
                v->iosurface = made, v->shadow = shadow, v->merged = merged, v->alloc = alloc, v->plane_count = pl_count;
                memcpy(v->plane, pl, pl_count * sizeof *pl);
                v->video_lock = OS_UNFAIR_LOCK_INIT, v->reader_lock = OS_UNFAIR_LOCK_INIT;
                v->held = alloc + (made ? IOSurfaceGetAllocSize(made) : 0);
                if (vk) planar_add(self, vk, v);
            } else {
                if (made) CFRelease(made);
                free(shadow);
                free(merged);
            }
        }
        surface->video = v;
        if (!v) {
            NSMutableString *m = [NSMutableString stringWithFormat:@"surface %llu: no IOSurface for its planes (alloc %llu):", mapping, alloc];
            for (int i = 0; i < 96; i++) [m appendFormat:@" %08x", ((const uint32_t *)desc)[i]];
            [self complain:m];
        }
        sync_iosurface(self, surface);
    }
    free(pages);
    surfaces_made++;
    fprintf(stderr, "hengevm: pvg-metal: surface %llu: %llux%llu, row %llu, %llu KB, fourcc %c%c%c%c planes %u\n",
            mapping, (unsigned long long)sw, (unsigned long long)sh, row, alloc >> 10, desc[7], desc[6], desc[5], desc[4], planes);
    return surface;
}

- (HvObject *)currentSurface:(uint64_t)mapping {
    HvObject *surface = surfaces[@(mapping)];
    if (surface && hv_pvg_lite_old_driver(lite)) {
        const uint8_t *now = hv_pvg_lite_surface(lite, mapping);
        if (now && (now[0] | now[0x10]) && memcmp(surface->described, now, sizeof surface->described)) {
            [surfaces removeObjectForKey:@(mapping)];
            surface = nil;
        }
    }
    return surface ? surface : [self newSurface:mapping];
}

/* A surface (IOSurface in the guest): a texture over its guest pages, so the
   guest's CPU and the GPU see the same pixels, as they do through a real one.
   {mapping id, tag, size, ref, description}. Its descriptor: page list at +0
   (physical >> 12, bit 0 present, one per 16 KB), first-page offset +0x60,
   allocation +0x18, bytes per row +0x30. */
- (void)makeMappedTexture:(HvObject *)o at:(uint64_t)at task:(uint32_t)task {
    /* d holds a v2 description (tag 0x39, 0x38 bytes) whose swizzle sits at +0x23..0x26: with only 32 bytes
       read those came from the stack, and macOS 27.2's first surface made Metal abort Henge (swizzle 193). */
    struct __attribute__((packed)) { uint64_t mapping; uint32_t tag, size, ref; uint8_t d[64]; } h;
    memset(&h, 0, sizeof h);
    const uint32_t head = (uint32_t)offsetof(__typeof__(h), d);
    if (hv_pvg_lite_task_read(lite, task, at, &h, head + 32)) return;
    /* size is the record's length from its tag (0x38 for a v2 one, 0x30 for the others), as Apple's host reads it:
       the description, then the plane and rotation (hv_pvg_video_record_plane) */
    if (h.size > head - 8 + 32 && hv_pvg_lite_task_read(lite, task, at, &h, (uint32_t)MIN(8 + (uint64_t)h.size, sizeof h))) {
        memset(h.d + 32, 0, sizeof h.d - 32); /* past the end of the task: keep what the first read had (plane 0) */
    }
    o->on_mapping = h.mapping;
    if (h.mapping < HV_PVG_MAPPING_IDS && (unmapped[h.mapping / 8] >> (h.mapping % 8) & 1)) {
        /* unmapped: what its descriptor names may be the guest's other memory now */
        static int told;
        if (told++ < 8) fprintf(stderr, "hengevm: pvg-metal: surface %llu used after the guest unmapped it: not drawn\n", h.mapping);
        o->gone = YES; /* tried again once it is mapped again (object:task:) */
        return;
    }
    HvObject *surface = [self currentSurface:h.mapping];
    if (!surface) surface = [self newSurface:h.mapping];
    if (!surface) return;
    MTLTextureDescriptor *t = h.tag == 0x39 ? descriptor_v2(h.d) : descriptor_v1(h.d);
    t.storageMode = MTLStorageModeShared;
    set_framebuffer_only(t, NO);
    t.usage |= MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    /* Metal aborts on a swizzled render target, and a surface is always drawn into here: views over it
       (tags 7/8/0x1b) can still swizzle. */
    MTLTextureSwizzleChannels sw = t.swizzle;
    if (sw.red != MTLTextureSwizzleRed || sw.green != MTLTextureSwizzleGreen || sw.blue != MTLTextureSwizzleBlue || sw.alpha != MTLTextureSwizzleAlpha) {
        static int told;
        if (told++ < 3) fprintf(stderr, "hengevm: pvg-metal: surface %llu asked for swizzle %u %u %u %u; drawing it unswizzled\n", h.mapping, sw.red, sw.green, sw.blue, sw.alpha);
        t.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
    }
    o->surface = surface;
    over_surface(o);
    HvVideo *v = surface->video;
    if (v) {
        /* The plane the record names, if the view fits it */
        uint32_t named = hv_pvg_video_record_plane(h.tag, h.d);
        int plane = hv_pvg_video_view_plane(v->plane, v->plane_count, named, t.width, t.height, (uint32_t)t.pixelFormat);
        BOOL whole = plane == 0 && !hv_pvg_linear_bpp((uint32_t)t.pixelFormat); /* a YCbCr format: the whole surface */
        if (plane < 0)
            [self complain:[NSString stringWithFormat:@"surface %llu: format %lu %lux%lu on plane %u of %u: %s", h.mapping, (unsigned long)t.pixelFormat,
                            (unsigned long)t.width, (unsigned long)t.height, named, v->plane_count, plane == -1 ? "no such plane" : "doesn't fit it"]];
        else if (t.textureType != MTLTextureType2D || t.mipmapLevelCount != 1 || t.sampleCount != 1 ||
                 (whole && (t.width > IOSurfaceGetWidth(v->iosurface) || t.height > IOSurfaceGetHeight(v->iosurface))))
            [self complain:[NSString stringWithFormat:@"surface %llu: a %lux%lu type %lu texture over its planes", h.mapping, (unsigned long)t.width,
                            (unsigned long)t.height, (unsigned long)t.textureType]];
        else if (![self refuse:t what:@"video surface texture"]) {
            o->object = [device newTextureWithDescriptor:t iosurface:v->iosurface plane:(NSUInteger)plane];
            if (!o->object) { /* video formats cannot be drawn into */
                t.usage = MTLTextureUsageShaderRead;
                o->object = [device newTextureWithDescriptor:t iosurface:v->iosurface plane:(NSUInteger)plane];
            }
        }
    }
    if (!o->object && surface->object) {
        NSUInteger req_row = surface->row >= t.width * pixel_bytes(t.pixelFormat) ? surface->row : t.width * pixel_bytes(t.pixelFormat);
        NSUInteger align = [device minimumLinearTextureAlignmentForPixelFormat:t.pixelFormat];
        if (align && (req_row % align)) req_row = (req_row + align - 1) & ~(align - 1);
        if (linear_texture_fits(device, t, surface->first, req_row, [(id<MTLBuffer>)surface->object length]))
            o->object = [(id<MTLBuffer>)surface->object newTextureWithDescriptor:t offset:surface->first bytesPerRow:req_row];
    }
    if (!o->object) {
        [self complain:[NSString stringWithFormat:@"surface %llu: format %lu row %llu not possible", h.mapping, (unsigned long)t.pixelFormat, surface->row]];
        t.storageMode = MTLStorageModePrivate;
        if (![self refuse:t what:@"surface stand-in"]) o->object = [device newTextureWithDescriptor:t]; /* something to bind: it stays empty */
    }
}

/* The ref's entry in its task's object table, and the object made for it if that is still what the entry says (else
   nil). NO if there is no entry to read. */
- (BOOL)entry:(uint32_t)ref task:(uint32_t)task into:(uint32_t[3])e kept:(HvObject **)kept {
    *kept = nil;
    uint64_t list = hv_pvg_lite_task_objects(lite, task);
    if (!ref || !list || hv_pvg_lite_task_read(lite, task, list + (uint64_t)ref * 12, e, 12)) return NO;
    HvObject *o = objs_get(task_objects(self, task, NO), ref);
    if (o && !memcmp(o->entry, e, 12) && !o->gone && !(o->surface && o->surface->gone)) { /* over an unmapped surface: made anew */
        /* macOS 14's (and Ventura's) driver names a surface before it writes the surface's description: try again a while.
           And it reuses surface ids: a texture over a surface that has since been replaced is made again. */
        BOOL replaced = o->object && (e[0] & 0xff) == OBJ_MAPPED_TEXTURE && hv_pvg_lite_old_driver(lite) &&
                        o->surface != [self currentSurface:o->on_mapping];
        if (replaced) o->retries = 0;
        if ((!o->object || replaced) && (e[0] & 0xff) == OBJ_MAPPED_TEXTURE && o->retries < 600) {
            return YES;
        }
        if (!o->follows) *kept = o;
        else {
            BOOL current = !o->follows->mapped || refresh_pages(self, task, o->follows); /* else made anew, over it made anew */
            if (current && o->follows->generation == o->follows_generation) *kept = o;
        }
    }
    return YES;
}

/* A view's parent texture, and its parent's, as far as they are views not yet made: each made from the far end of the
   chain back, so each finds its own parent made and making it goes one object deep, not one per view in the chain (a
   guest's chain is as long as it likes, and the worker's stack is half a megabyte). A chain that comes back on itself
   is views of nothing: each in the loop is kept empty, as a view of a view being made is (object:task:). */
- (HvObject *)viewParent:(uint32_t)ref task:(uint32_t)task {
    NSMutableArray<NSNumber *> *chain = [NSMutableArray array];
    NSMutableDictionary<NSNumber *, NSNumber *> *where = [NSMutableDictionary dictionary];
    for (uint32_t at_ref = ref;;) {
        uint32_t e[3];
        HvObject *kept;
        if (![self entry:at_ref task:task into:e kept:&kept] || kept) break;
        NSNumber *loop = where[@(at_ref)];
        if (loop) {
            HvTaskObjects *objs = task_objects(self, task, YES); /* entry: read it, so task < HV_TASKS */
            for (NSUInteger i = loop.unsignedIntegerValue; i < chain.count; i++) {
                uint32_t looped = chain[i].unsignedIntValue, le[3];
                HvObject *none = [HvObject new], *had;
                if (![self entry:looped task:task into:le kept:&had]) continue;
                memcpy(none->entry, le, 12);
                objs_set(objs, looped, none);
            }
            [self complain:@"texture views in a loop: views of nothing"];
            [chain removeObjectsInRange:NSMakeRange(loop.unsignedIntegerValue, chain.count - loop.unsignedIntegerValue)];
            break;
        }
        uint32_t type = e[0] & 0xff;
        if (type != OBJ_LINEAR_TEXTURE && type != 9) break;
        struct __attribute__((packed)) { uint32_t tag, size, ref, parent; } h;
        if (hv_pvg_lite_task_read(lite, task, (uint64_t)e[2] << 32 | e[1], &h, sizeof h) || !(h.tag == 7 || h.tag == 8 || h.tag == 0x1b))
            break;
        where[@(at_ref)] = @(chain.count);
        [chain addObject:@(at_ref)];
        at_ref = h.parent;
    }
    for (NSUInteger i = chain.count; i > 1; i--) [self object:chain[i - 1].unsignedIntValue task:task];
    return [self object:ref task:task kind:KIND_TEXTURE];
}

- (HvObject *)object:(uint32_t)ref task:(uint32_t)task {
    uint32_t e[3];
    HvObject *o;
    if (![self entry:ref task:task into:e kept:&o]) return nil;
    if (o) return o;
    HvTaskObjects *objs = task_objects(self, task, YES); /* entry: read it, so task < HV_TASKS */
    HvObject *old = objs ? objs_get(objs, ref) : nil;
    uint32_t tries = old ? old->retries + 1 : 0;
    o = [HvObject new];
    memcpy(o->entry, e, 12);
    o->retries = tries;
    o->cache_task = task, o->cache_ref = ref;
    if (!objs || !objs_set(objs, ref, o)) {
        [self complain:@"no memory for a task's object table"];
        return nil;
    }
    uint32_t type = e[0] & 0xff, size = e[0] >> 8;
    uint64_t at = (uint64_t)e[2] << 32 | e[1];
    HvKind serialized = KIND_NONE;
    switch (type) {
    case OBJ_BUFFER: [self makeBuffer:o at:at task:task]; break;
    case OBJ_TEXTURE: case 3: /* 3: macOS 13's driver, the same {length, page, layout, mips, record} entry */
        [self makeTexture:o at:at size:size task:task]; break;
    case OBJ_FUNCTION: o->object = [self makeFunction:at task:task]; break;
    case OBJ_SERIALIZED: o->object = [self makeSerialized:at length:size task:task kind:&serialized into:o]; break;
    case OBJ_LINEAR_TEXTURE: case 9: [self makeLinearTexture:o at:at size:size task:task]; break;
    case 13: [self makeHeap:o at:at task:task]; break;
    case 14: [self makeHeapBuffer:o at:at task:task]; break;
    case 19: /* macOS 27: a buffer, with the same {length, page, flags} entry as type 1 (16 KB ones seen). The guest
                has object info (tags 450...) written into these, so leaving them out broke its argument buffers:
                black layers, and "info 450 ... no buffer" over and over */
        [self makeBuffer:o at:at task:task];
        break;
    case 0: break; /* an empty slot */
    case OBJ_MAPPED_TEXTURE: [self makeMappedTexture:o at:at task:task]; break;
    default: {
        uint32_t w[24] = {0};
        hv_pvg_lite_task_read(lite, task, at, w, sizeof w);
        NSMutableString *m = [NSMutableString stringWithFormat:@"object type %u size %u:", type, size];
        for (int i = 0; i < 24; i++) [m appendFormat:@" %08x", w[i]];
        [self complain:m];
        break;
    }
    }
    o->kind = !o->object ? KIND_NONE
            : type == OBJ_SERIALIZED ? serialized
            : type == OBJ_BUFFER || type == 19 || type == 14 ? KIND_BUFFER
            : type == OBJ_TEXTURE || type == 3 || type == OBJ_LINEAR_TEXTURE || type == 9 || type == OBJ_MAPPED_TEXTURE ? KIND_TEXTURE
            : type == OBJ_FUNCTION ? KIND_FUNCTION
            : type == 13 ? KIND_HEAP : KIND_NONE;
    if (o->kind == KIND_NONE) o->object = nil;
    if (o->object && tries > 0) {
        static int told;
        if (told++ < 30) fprintf(stderr, "hengevm: pvg-metal: task %u ref %u: texture made on try %u\n", task, ref, tries);
    }
    return o;
}

/* The object a ref names, if it is one of these kinds (a mask of 1 << HvKind), else nil, and told */
- (HvObject *)object:(uint32_t)ref task:(uint32_t)task kinds:(uint32_t)kinds {
    HvObject *o = [self object:ref task:task];
    if (!o || !o->object) return nil;
    if (kinds >> o->kind & 1) return o;
    const char *want = "something else";
    for (int k = 1; k < KIND_COUNT; k++)
        if (kinds >> k & 1) { want = kind_names[k]; break; }
    [self complain:[NSString stringWithFormat:@"a %s used as a %s", kind_names[o->kind], want]];
    return nil;
}

- (HvObject *)object:(uint32_t)ref task:(uint32_t)task kind:(HvKind)kind {
    return [self object:ref task:task kinds:1u << kind];
}

- (id)get:(uint32_t)ref task:(uint32_t)task kind:(HvKind)kind {
    HvObject *o = [self object:ref task:task kinds:1u << kind];
    return o ? o->object : nil;
}

/* The object's guest memory as a buffer, once it has some. */
static id<MTLBuffer> guest_copy(HvReplay *r, uint32_t task, HvObject *o) {
    if (!o->vpage || !o->length) return nil;
    if (!o->mapped && !map_pages(r, task, o)) return nil;
    if (!refresh_pages(r, task, o)) return nil;
    if (!o->guest_copy) o->guest_copy = buffer_over_mapping(r, o);
    return o->guest_copy;
}

/* Some of its pages are guest RAM, as mapped: not from o->pages, which keeps the guest's numbers for pages outside RAM
   too (map_pages), so an object all outside RAM would have the GPU's copy overwritten by zeros (page:toGuest:NO) */
static BOOL has_guest_pages(HvObject *o) { return o->pages && o->pages_in_ram; }

/* Between guest memory and the GPU's own copy, in the layout the guest was told. */
- (void)page:(HvObject *)o toGuest:(BOOL)down task:(uint32_t)task blit:(id<MTLBlitCommandEncoder> *)blit commandBuffer:(id<MTLCommandBuffer>)cb {
    id<MTLBuffer> copy = guest_copy(self, task, o);
    if (!copy || !has_guest_pages(o)) return;
    if (o->heap_whole) {
        if (!*blit) *blit = [cb blitCommandEncoder];
        NSUInteger bytes = MIN(copy.length, o->heap_whole.length);
        if (down) [*blit copyFromBuffer:o->heap_whole sourceOffset:0 toBuffer:copy destinationOffset:0 size:bytes];
        else [*blit copyFromBuffer:copy sourceOffset:0 toBuffer:o->heap_whole destinationOffset:0 size:bytes];
        return;
    }
    id<MTLTexture> tex = o->object;
    if (!tex || !o->tex_layout || tex.sampleCount > 1 || tex.isFramebufferOnly) return;
    /* makeTexture checked the table: each level and slice the texture has (a cube's faces counted), at its size,
       within the memory (hv_pvg_layout_problem) */
    if (!*blit) *blit = [cb blitCommandEncoder];
    for (uint32_t s = 0; s < o->tex_slices; s++)
        for (uint32_t m = 0; m < o->tex_mips; m++) {
            const uint8_t *mip = o->tex_layout + m * 36;
            uint64_t off, row;
            uint32_t w, h, d;
            memcpy(&off, mip, 8), memcpy(&row, mip + 0x10, 8), memcpy(&w, mip + 0x18, 4), memcpy(&h, mip + 0x1c, 4), memcpy(&d, mip + 0x20, 4);
            uint32_t bh = o->tex_block_h ? o->tex_block_h : 1;
            uint64_t from = o->tex_base + o->tex_stride * s + off;
            uint64_t image = row * ((h + bh - 1) / bh);
            if (from + image * d > o->length) continue;
            if (down)
                [*blit copyFromTexture:tex sourceSlice:s sourceLevel:m sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, d)
                              toBuffer:copy destinationOffset:from destinationBytesPerRow:row destinationBytesPerImage:image];
            else
                [*blit copyFromBuffer:copy sourceOffset:from sourceBytesPerRow:row sourceBytesPerImage:image
                           sourceSize:MTLSizeMake(w, h, d) toTexture:tex destinationSlice:s destinationLevel:m destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
}

/* The guest asks to see what the GPU made: {task, count, refs}. */
- (void)synchronize:(const uint32_t *)a length:(uint32_t)n {
    if (n < 8) return;
    uint32_t task = a[0], count = a[1];
    if (n < 8 + count * 4ull) return;
    HvTaskObjects *objs = task_objects(self, task, NO);
    id<MTLCommandBuffer> cb = nil;
    id<MTLBlitCommandEncoder> blit = nil;
    for (uint32_t i = 0; i < count; i++) {
        HvObject *o = objs_get(objs, a[2 + i]);
        if (o && o->surface && o->surface->gone) continue; /* unmapped: its pages may be the guest's other memory now */
        if (o && o->surface && o->surface->video && o->surface->video->host_newer) {
            if (!cb) cb = [queue commandBuffer];
            back_once(writeback, o->surface);
        }
        if (!o || !o->guest_backed || !o->host_valid) continue;
        if (!cb) cb = [queue commandBuffer];
        [self page:o toGuest:YES task:task blit:&blit commandBuffer:cb];
        o->guest_stale = NO;
    }
    [blit endEncoding];
    if (cb) {
        [self commit:cb channel:0 stamp:0 complete:NO redraw:NO];
        synced++;
    }
}

/* {task, count, {ref, host invalid, host valid, guest invalid, guest valid}}. */
- (void)invalidate:(const uint32_t *)a length:(uint32_t)n {
    if (n < 8 || n < 8 + a[1] * 8ull) return;
    HvTaskObjects *objs = task_objects(self, a[0], NO);
    const uint8_t *e = (const uint8_t *)(a + 2);
    for (uint32_t i = 0; objs && i < a[1]; i++, e += 8) {
        HvObject *o = objs_get(objs, *(const uint32_t *)e);
        if (!o) continue;
        if (e[6]) o->guest_stale = YES;
        if (e[7]) o->guest_stale = NO;
        if (e[4]) o->host_valid = NO;
        if (e[5]) o->host_valid = YES;
    }
}

/* A record's bytes, as a pointer into the buffer's private copy. */
typedef struct {
    const uint8_t *p;
    uint32_t size;
} Rec;

#define U32(off) (*(const uint32_t *)(r.p + (off)))
#define U16(off) (*(const uint16_t *)(r.p + (off)))
#define U64(off) ({ uint64_t _v; memcpy(&_v, r.p + (off), 8); _v; })

/* Where an attachment's texture lies in the texture its view is of (a view of a view counted through), so two
   attachments on the same pixels can be seen */
static void attach_root(HvPassAttach *a, id<MTLTexture> t) {
    uint32_t level = 0, slice = 0;
    while (t.parentTexture) level += (uint32_t)t.parentRelativeLevel, slice += (uint32_t)t.parentRelativeSlice, t = t.parentTexture;
    a->root = (uint64_t)(uintptr_t)(__bridge void *)t, a->root_level = level, a->root_slice = slice;
}

/* An attachment of the pass record at o: {texture, resolve texture, then 16 bits each: level, slice, depth plane,
   resolve level, slice, depth plane, load, store, store options} */
- (void)attach:(HvPassAttach *)a texture:(id<MTLTexture> __strong *)tex resolve:(id<MTLTexture> __strong *)res rec:(Rec)r at:(uint32_t)o task:(uint32_t)task {
    *tex = [self get:U32(o) task:task kind:KIND_TEXTURE];
    *res = *tex ? [self get:U32(o + 4) task:task kind:KIND_TEXTURE] : nil;
    a->present = *tex != nil, a->has_resolve = *res != nil;
    if (*tex) a->tex = made_desc(*tex), attach_root(a, *tex), a->texture = (uint64_t)(uintptr_t)(__bridge void *)*tex;
    if (*res) a->resolve = made_desc(*res), a->resolve_texture = (uint64_t)(uintptr_t)(__bridge void *)*res;
    a->level = U16(o + 8), a->slice = U16(o + 10), a->depth_plane = U16(o + 12);
    a->resolve_level = U16(o + 14), a->resolve_slice = U16(o + 16), a->resolve_depth_plane = U16(o + 18);
    a->load = U16(o + 20), a->store = U16(o + 22), a->options = U16(o + 24);
}

static void set_attachment(MTLRenderPassAttachmentDescriptor *d, const HvPassAttach *a, id<MTLTexture> tex, id<MTLTexture> res) {
    d.texture = a->present ? tex : nil;
    d.resolveTexture = a->has_resolve ? res : nil;
    d.level = a->level, d.slice = a->slice, d.depthPlane = a->depth_plane;
    d.resolveLevel = a->resolve_level, d.resolveSlice = a->resolve_slice, d.resolveDepthPlane = a->resolve_depth_plane;
    d.loadAction = (MTLLoadAction)a->load, d.storeAction = (MTLStoreAction)a->store;
    d.storeActionOptions = (MTLStoreActionOptions)a->options;
}

/* Metal stops the app on a pass it can't begin: every part of it is checked first (hv_pvg_pass_problem), and a pass
   that fails is told and dropped, its commands with it. */
- (id<MTLRenderCommandEncoder>)beginPass:(Rec)r task:(uint32_t)task commandBuffer:(id<MTLCommandBuffer>)cb {
    HvPass p;
    memset(&p, 0, sizeof p);
    id<MTLTexture> tex[10], res[10]; /* depth, stencil, colours */
    [self attach:&p.depth texture:&tex[0] resolve:&res[0] rec:r at:0 task:task];
    memcpy(&p.clear_depth, r.p + 0x1c, 8);
    [self attach:&p.stencil texture:&tex[1] resolve:&res[1] rec:r at:0x28 task:task];
    double clear[8][4];
    memset(clear, 0, sizeof clear);
    for (int i = 0; i < 8; i++) {
        uint32_t o = 0x4c + (uint32_t)i * 0x3c;
        if (o + 0x3c > r.size) break;
        [self attach:&p.color[i] texture:&tex[2 + i] resolve:&res[2 + i] rec:r at:o task:task];
        memcpy(clear[i], r.p + o + 0x1c, sizeof clear[i]);
    }
    id<MTLBuffer> visibility = nil;
    if (r.size >= 0x248) { /* after the attachments: the visibility buffer and the render target's size */
        if (U32(0x22c)) visibility = [self get:U32(0x22c) task:task kind:KIND_BUFFER];
        p.layers = U64(0x230), p.width = U64(0x238), p.height = U64(0x240);
    }
    /* What came before the pass itself. */
    p.raster_samples = pre_raster_samples, p.imageblock = pre_imageblock, p.threadgroup = pre_threadgroup;
    p.tile_w = pre_tile_w, p.tile_h = pre_tile_h;
    pre_raster_samples = pre_imageblock = pre_threadgroup = pre_tile_w = pre_tile_h = 0;
    int named = 0, made = 0;
    for (int i = -2; i < 8; i++) {
        uint32_t o = i == -2 ? 0 : i == -1 ? 0x28 : 0x4c + (uint32_t)i * 0x3c;
        if (o + 4 > r.size || !U32(o)) continue;
        named++;
        made += (i == -2 ? tex[0] : i == -1 ? tex[1] : tex[2 + i]) != nil;
    }
    if (named && made < named) {
        static int told;
        if (told++ < 20) fprintf(stderr, "hengevm: pvg-metal: pass skipped: %d of its %d attachments were never made\n", named - made, named);
        return nil;
    }
    const char *why = hv_pvg_pass_problem(&p, caps);
    if (why) {
        NSMutableString *m = [NSMutableString stringWithFormat:@"render pass dropped: %s;", why];
        HvPassAttach *all[10] = {&p.depth, &p.stencil};
        for (int i = 0; i < 8; i++) all[2 + i] = &p.color[i];
        for (int i = 0; i < 10; i++)
            if (all[i]->present)
                [m appendFormat:@" %s%d fmt %u type %u %ux%u x%u, %u samples, level %u slice %u, load %u store %u options %u;", i < 2 ? "ds" : "c", i < 2 ? i : i - 2,
                                all[i]->tex.format, all[i]->tex.type, all[i]->tex.width, all[i]->tex.height, all[i]->tex.array, all[i]->tex.samples,
                                all[i]->level, all[i]->slice, all[i]->load, all[i]->store, all[i]->options];
        [m appendFormat:@" %llux%llu x%llu, tile %ux%u, samples %u", p.width, p.height, p.layers, p.tile_w, p.tile_h, p.raster_samples];
        [self complain:m];
        return nil;
    }
    memset(&pass_targets, 0, sizeof pass_targets);
    pass_targets.samples = p.raster_samples ? p.raster_samples : 1;
    for (int i = 0; i < 10; i++) {
        const HvPassAttach *a = i == 0 ? &p.depth : i == 1 ? &p.stencil : &p.color[i - 2];
        if (!a->present) continue;
        if (i == 0) pass_targets.depth = a->tex.format;
        else if (i == 1) pass_targets.stencil = a->tex.format;
        else pass_targets.color[i - 2] = a->tex.format;
        if (!p.raster_samples) pass_targets.samples = a->tex.samples ? a->tex.samples : 1;
    }
    if (!pass_desc) pass_desc = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassDescriptor *d = pass_desc;
    set_attachment(d.depthAttachment, &p.depth, tex[0], res[0]);
    d.depthAttachment.clearDepth = p.clear_depth;
    set_attachment(d.stencilAttachment, &p.stencil, tex[1], res[1]);
    d.stencilAttachment.clearStencil = U32(0x44);
    pass_w = p.width ? p.width : UINT64_MAX, pass_h = p.height ? p.height : UINT64_MAX, pass_layers = p.layers;
    for (int i = 0; i < 10; i++) {
        const HvPassAttach *a = i == 0 ? &p.depth : i == 1 ? &p.stencil : &p.color[i - 2];
        if (!a->present) continue;
        uint64_t s[3];
        hv_pvg_level_size(&a->tex, a->level, s);
        if (s[0] < pass_w) pass_w = s[0];
        if (s[1] < pass_h) pass_h = s[1];
    }
    uint64_t targets = 0, samples = p.raster_samples ? p.raster_samples : 1;
    for (int i = 0; i < 10; i++) {
        const HvPassAttach *a = i == 0 ? &p.depth : i == 1 ? &p.stencil : &p.color[i - 2];
        if (a->present) targets++, samples = a->tex.samples > samples ? a->tex.samples : samples;
    }
    /* a pass loads, clears and stores its targets' pixels: work like any other */
    uint64_t pixels = hv_pvg_sat_mul(hv_pvg_sat_mul(pass_w, pass_h), hv_pvg_sat_mul(p.layers ? p.layers : 1, hv_pvg_sat_mul(targets ? targets : 1, samples)));
    if (![self charge:pixels]) return nil;
    for (int i = 0; i < 8; i++) {
        MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
        set_attachment(c, &p.color[i], tex[2 + i], res[2 + i]);
        if (p.color[i].present) c.clearColor = MTLClearColorMake(clear[i][0], clear[i][1], clear[i][2], clear[i][3]);
    }
    d.visibilityResultBuffer = visibility;
    d.renderTargetArrayLength = (NSUInteger)p.layers;
    d.renderTargetWidth = (NSUInteger)p.width;
    d.renderTargetHeight = (NSUInteger)p.height;
    d.defaultRasterSampleCount = p.raster_samples;
    d.imageblockSampleLength = p.imageblock;
    d.threadgroupMemoryLength = p.threadgroup;
    d.tileWidth = p.tile_w, d.tileHeight = p.tile_h;
    if (tracing) {
        id<MTLTexture> c0 = d.colorAttachments[0].texture;
        MTLClearColor cc = d.colorAttachments[0].clearColor;
        HvObject *co = [self object:U32(0x4c) task:task];
        fprintf(stderr, "trace: pass c0 ref %u %lux%lu fmt %lu load %lu store %lu clear %.2f %.2f %.2f %.2f surface %s heap %d\n", U32(0x4c), (unsigned long)c0.width, (unsigned long)c0.height, (unsigned long)c0.pixelFormat, (unsigned long)d.colorAttachments[0].loadAction, (unsigned long)d.colorAttachments[0].storeAction, cc.red, cc.green, cc.blue, cc.alpha, co && co->surface ? "yes" : "no", c0.heap != nil);
    }
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
    if (!e) {
        NSMutableString *m = [NSMutableString stringWithString:@"no render encoder:"];
        for (int i = 0; i < 8; i++) {
            MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
            uint32_t ref = U32(0x4c + (uint32_t)i * 0x3c);
            if (ref) [m appendFormat:@" c%d ref %u %s %lux%lu fmt %lu load %lu store %lu usage %lu", i, ref, c.texture ? "ok" : "MISSING", (unsigned long)c.texture.width, (unsigned long)c.texture.height, (unsigned long)c.texture.pixelFormat, (unsigned long)c.loadAction, (unsigned long)c.storeAction, (unsigned long)c.texture.usage];
        }
        [m appendFormat:@" depth ref %u stencil ref %u", U32(0), U32(0x28)];
        [self complain:m];
    }
    return e;
}

/* One serialized call. Tags are the guest's. */
- (void)useHeaps:(const uint32_t *)refs count:(uint32_t)n task:(uint32_t)task encoder:(id)e stages:(int)stages {
    id<MTLHeap> __unsafe_unretained list[64];
    NSMutableArray *keep = [NSMutableArray new];
    uint32_t have = 0;
    for (uint32_t i = 0; i < n && have < 64; i++) {
        id heap = [self get:refs[i] task:task kind:KIND_HEAP];
        if (!heap) continue;
        [keep addObject:heap];
        list[have++] = heap;
    }
    if (!have) return;
    if (stages >= 0) [(id<MTLRenderCommandEncoder>)e useHeaps:list count:have stages:(MTLRenderStages)stages];
    else if ([e conformsToProtocol:@protocol(MTLRenderCommandEncoder)]) [(id<MTLRenderCommandEncoder>)e useHeaps:list count:have];
    else [(id<MTLComputeCommandEncoder>)e useHeaps:list count:have];
}

/* The buffers of a {index, count}, {ref, offset[, stride]}... list: each a buffer, and its offset within it (else none
   is bound there, told). bound: the lengths, for a later offset alone. */
- (void)buffers:(Rec)r task:(uint32_t)task stride:(uint32_t)stride into:(__unsafe_unretained id<MTLBuffer> *)buffers strong:(id __strong *)strong
        offsets:(NSUInteger *)offsets strides:(NSUInteger *)strides bound:(uint64_t *)lens {
    uint32_t index = U32(0), count = U32(4); /* checked against the encoder's slots (hv_pvg_command_problem) */
    for (uint32_t i = 0; i < count; i++) {
        id<MTLBuffer> b = [self get:U32(8 + i * stride) task:task kind:KIND_BUFFER];
        uint64_t off = U64(12 + i * stride);
        if (b && off >= b.length) {
            [self complain:[NSString stringWithFormat:@"a buffer bound at %llu of its %lu bytes", off, (unsigned long)b.length]];
            b = nil;
        }
        strong[i] = b, buffers[i] = b;
        offsets[i] = b ? (NSUInteger)off : 0;
        if (strides) strides[i] = (NSUInteger)U64(20 + i * stride);
        lens[index + i] = b ? b.length : 0;
    }
}

/* A bound buffer's new offset: within it, or not done */
- (BOOL)offset:(uint64_t)off of:(const uint64_t *)lens index:(uint32_t)index {
    if (lens[index] && off < lens[index]) return YES;
    [self complain:[NSString stringWithFormat:@"buffer offset %llu at a slot with %llu bytes bound", off, lens[index]]];
    return NO;
}

/* count indexes of type (0: 16 bits, 1: 32) from off, all in the buffer */
- (BOOL)indexes:(id<MTLBuffer>)b type:(uint32_t)type offset:(uint64_t)off count:(uint64_t)count {
    uint64_t size = type ? 4 : 2;
    if (b && off % size == 0 && off <= b.length && count <= (b.length - off) / size) return YES;
    if (b) [self complain:[NSString stringWithFormat:@"draw of %llu indexes at %llu of a %lu-byte buffer", count, off, (unsigned long)b.length]];
    return NO;
}

/* An indirect draw's arguments (size bytes) at off, in the buffer */
- (BOOL)arguments:(id<MTLBuffer>)b offset:(uint64_t)off size:(uint64_t)size {
    if (b && off % 4 == 0 && off <= b.length && b.length - off >= size) return YES;
    if (b) [self complain:[NSString stringWithFormat:@"indirect arguments at %llu of a %lu-byte buffer", off, (unsigned long)b.length]];
    return NO;
}

/* A patch's or control point's index buffer: none, or indexes of size bytes (4 for patches; the pipeline's for control
   points) from an aligned offset within it */
- (BOOL)patchIndexes:(id<MTLBuffer>)b offset:(uint64_t)off size:(uint64_t)size {
    if (!b || (off % size == 0 && off < b.length)) return YES;
    [self complain:[NSString stringWithFormat:@"patch indexes at %llu of a %lu-byte buffer", off, (unsigned long)b.length]];
    return NO;
}

/* A direct patch draw's indexes all within its index buffers (hv_pvg_patch_ids_problem): the buffers' offsets first
   (patchIndexes:offset:size:), then the patches start to start + count, and cps control point indexes for each, of the
   pipeline's size */
- (BOOL)patchDraw:(uint32_t)tag start:(uint64_t)start count:(uint64_t)count points:(uint64_t)cps
          patches:(id<MTLBuffer>)patches at:(uint64_t)poff indexes:(id<MTLBuffer>)points at:(uint64_t)coff {
    if (![self patchIndexes:patches offset:poff size:4] || ![self patchIndexes:points offset:coff size:pipeline_cpsize]) return NO;
    const char *why = hv_pvg_patch_ids_problem(start, count, cps, patches != nil, patches ? patches.length - poff : 0,
                                               points != nil, points ? points.length - coff : 0, pipeline_cpsize);
    if (!why) return YES;
    [self complain:[NSString stringWithFormat:@"draw %u dropped: %s (patches %llu to %llu, %llu control points; buffers of %lu at %llu, %lu at %llu)",
                    tag, why, (unsigned long long)start, (unsigned long long)start + count, (unsigned long long)cps,
                    (unsigned long)patches.length, (unsigned long long)poff, (unsigned long)points.length, (unsigned long long)coff]];
    return NO;
}

/* A patch draw's tessellation factors all within the factor buffer (hv_pvg_tess_fit), for the pipeline's patches
   (if it says nothing: 12 bytes each, the larger, and for each patch and instance) */
- (BOOL)factors:(uint32_t)tag start:(uint64_t)start count:(uint64_t)count instances:(uint64_t)inst base:(uint64_t)first indexed:(BOOL)indexed {
    uint64_t c = count, i = inst;
    hv_pvg_tess_fit(pipeline_frec ? pipeline_frec : 12, tess_buf.length - tess_off, tess_stride, pipeline_frec ? pipeline_fstep : 3, indexed,
                    start, first, &c, &i);
    if (c == count && i == inst) return YES;
    uint32_t step = pipeline_frec ? pipeline_fstep : 3;
    if (indexed && (step == 1 || step == 3)) {
        [self complain:[NSString stringWithFormat:@"draw %u dropped: patch ids from the guest's index buffer, and tessellation factors for each patch", tag]];
        return NO;
    }
    [self complain:[NSString stringWithFormat:@"draw %u dropped: %llu patches x %llu instances past their tessellation factors", tag,
                                              (unsigned long long)count, (unsigned long long)inst]];
    return NO;
}

/* A scissor within the pass's render target (Metal stops the app on one past it) */
- (MTLScissorRect)scissor:(MTLScissorRect)s {
    if (s.x > pass_w) s.x = (NSUInteger)pass_w;
    if (s.y > pass_h) s.y = (NSUInteger)pass_h;
    if (s.width > pass_w - s.x) s.width = (NSUInteger)(pass_w - s.x);
    if (s.height > pass_h - s.y) s.height = (NSUInteger)(pass_h - s.y);
    return s;
}

- (void)render:(id<MTLRenderCommandEncoder>)e tag:(uint32_t)tag rec:(Rec)r task:(uint32_t)task {
    BOOL draw = tag <= 19 || tag == 28 || tag == 29, tile = tag == 155 || tag == 162;
    BOOL patches = (tag >= 12 && tag <= 15) || tag == 18 || tag == 19;
    if ((draw || tile) && (!pipeline_set || pipeline_tile != tile)) return; /* Metal crashes on drawing without a pipeline, or the wrong kind */
    if (draw && patches != (pipeline_patches >= 0)) { /* patches with a pipeline that draws them, and only with one */
        [self complain:[NSString stringWithFormat:@"draw %u with a pipeline that %s patches", tag, patches ? "draws no" : "draws"]];
        return;
    }
    if (patches && pipeline_patches > 0) { /* as many control points as the pipeline's patches have (0: it didn't say) */
        uint32_t at = tag == 12 ? 44 : tag == 13 ? 14 : tag == 14 ? 56 : tag == 15 ? 20 : tag == 18 ? 24 : 36;
        if (U16(at) != (uint32_t)pipeline_patches) {
            [self complain:[NSString stringWithFormat:@"draw %u of %u control points with a pipeline of %d", tag, U16(at), pipeline_patches]];
            return;
        }
    }
    if (tracing && draw) fprintf(stderr, "trace:  draw tag %u\n", tag);
    switch (tag) {
    case 24: { /* {fence, stages} */
        id f = [self get:U32(0) task:task kind:KIND_FENCE];
        if (f) [e updateFence:f afterStages:(MTLRenderStages)(U32(4) & 0xffff)];
        break;
    }
    case 25: {
        id f = [self get:U32(0) task:task kind:KIND_FENCE];
        if (f) [e waitForFence:f beforeStages:(MTLRenderStages)(U32(4) & 0xffff)];
        break;
    }
    case 27: /* {count, stages}, the heaps */
        if (r.size >= 8 + U32(0) * 4ull) [self useHeaps:(const uint32_t *)(r.p + 8) count:U32(0) task:task encoder:e stages:U32(4) & 0xffff];
        break;
    case 134: /* {count}, the heaps */
        if (r.size >= 4 + U32(0) * 4ull) [self useHeaps:(const uint32_t *)(r.p + 4) count:U32(0) task:task encoder:e stages:-1];
        break;
    case 116: { /* Metal crashes on a missing pipeline, and on drawing without one */
        HvObject *po = [self object:U32(0) task:task kinds:1u << KIND_RENDER_PIPELINE | 1u << KIND_TILE_PIPELINE];
        id state = po ? po->object : nil;
        BOOL tile = po && po->kind == KIND_TILE_PIPELINE;
        const char *why = po ? targets_problem(&po->targets, &pass_targets, tile) : NULL;
        if (why) {
            [self complain:[NSString stringWithFormat:@"pipeline %u not set: %s not the pass's", U32(0), why]];
            state = nil;
        }
        if (state) [e setRenderPipelineState:state];
        pipeline_set = state != nil, pipeline_tile = tile, pipeline_patches = state ? po->patch_points : -1;
        pipeline_tess = state && po->tess_weight ? po->tess_weight : 1;
        pipeline_frec = state ? po->patch_rec : 0, pipeline_fstep = state ? po->patch_step : 3;
        pipeline_cpsize = state && po->patch_index == 2 ? 2 : 4;
        cur_render = state;
        if (tracing) fprintf(stderr, "trace:  pipeline %u %s\n", U32(0), [objc_getAssociatedObject(state, "hvname") UTF8String]);
        break;
    }
    case 125: case 110: case 157: case 165: { /* vertex, fragment, tile buffers: {index, count}, {ref, offset[, stride]}... */
        uint32_t index = U32(0), count = U32(4), stride = tag == 165 ? 20 : 12;
        __unsafe_unretained id<MTLBuffer> buffers[31];
        id strong[31];
        NSUInteger offsets[31], strides[31];
        [self buffers:r task:task stride:stride into:buffers strong:strong offsets:offsets strides:tag == 165 ? strides : NULL
                bound:bound[tag == 110 ? 1 : tag == 157 ? 2 : 0]];
        NSRange range = NSMakeRange(index, count);
        if (tracing) for (uint32_t i = 0; i < count; i++) { /* m4 fork: what the shaders are given */
            id<MTLBuffer> bb = strong[i];
            fprintf(stderr, "trace:  %s buf %u ref %u off %lu len %lu:", tag == 110 ? "frag" : tag == 157 ? "tile" : "vtx", index + i, U32(8 + i * stride),
                    (unsigned long)offsets[i], (unsigned long)bb.length);
            if (bb && bb.storageMode != MTLStorageModePrivate && offsets[i] < bb.length) {
                const float *f = (const float *)((const uint8_t *)bb.contents + offsets[i]);
                NSUInteger nf = MIN((bb.length - offsets[i]) / 4, (NSUInteger)96);
                for (NSUInteger k = 0; k < nf; k++) fprintf(stderr, " %g", f[k]);
            }
            fprintf(stderr, "\n");
        }
        if (tag == 125) [e setVertexBuffers:buffers offsets:offsets withRange:range];
        else if (tag == 110) [e setFragmentBuffers:buffers offsets:offsets withRange:range];
        else if (tag == 157) [e setTileBuffers:buffers offsets:offsets withRange:range];
        else [e setVertexBuffers:buffers offsets:offsets attributeStrides:strides withRange:range];
        break;
    }
    case 126: if ([self offset:U64(4) of:bound[0] index:U32(0)]) [e setVertexBufferOffset:(NSUInteger)U64(4) atIndex:U32(0)]; break;
    case 111: if ([self offset:U64(4) of:bound[1] index:U32(0)]) [e setFragmentBufferOffset:(NSUInteger)U64(4) atIndex:U32(0)]; break;
    case 166:
        if ([self offset:U64(4) of:bound[0] index:U32(0)]) [e setVertexBufferOffset:(NSUInteger)U64(4) attributeStride:(NSUInteger)U64(12) atIndex:U32(0)];
        break;
    case 114: case 161: case 129: { /* fragment, tile, vertex textures: {index, count}, refs */
        uint32_t index = U32(0), count = U32(4);
        __unsafe_unretained id<MTLTexture> items[128];
        id strong[128];
        for (uint32_t i = 0; i < count; i++) {
            strong[i] = [self get:U32(8 + i * 4) task:task kind:KIND_TEXTURE];
            items[i] = strong[i];
        }
        if (tracing) for (uint32_t i = 0; i < count; i++) {
            id<MTLTexture> x = strong[i];
            HvObject *xo = [self object:U32(8 + i * 4) task:task];
            fprintf(stderr, "trace:  tex %u ref %u %lux%lu fmt %lu type %u swz %d%d%d%d surface %s heap %d valid %d\n", index + i, U32(8 + i * 4), (unsigned long)x.width, (unsigned long)x.height, (unsigned long)x.pixelFormat, xo ? xo->entry[0] & 0xff : 0, x.swizzle.red, x.swizzle.green, x.swizzle.blue, x.swizzle.alpha, xo && xo->surface ? "yes" : "no", x.heap != nil, xo ? xo->host_valid : 0);
        }
        if (tag == 114) [e setFragmentTextures:items withRange:NSMakeRange(index, count)];
        else if (tag == 161) [e setTileTextures:items withRange:NSMakeRange(index, count)];
        else [e setVertexTextures:items withRange:NSMakeRange(index, count)];
        break;
    }
    case 112: case 127: { /* fragment, vertex samplers */
        uint32_t index = U32(0), count = U32(4);
        __unsafe_unretained id<MTLSamplerState> items[16];
        id strong[16];
        for (uint32_t i = 0; i < count; i++) {
            strong[i] = [self get:U32(8 + i * 4) task:task kind:KIND_SAMPLER];
            items[i] = strong[i];
        }
        if (tag == 112) [e setFragmentSamplerStates:items withRange:NSMakeRange(index, count)];
        else [e setVertexSamplerStates:items withRange:NSMakeRange(index, count)];
        break;
    }
    case 117: {
        MTLScissorRect s;
        memcpy(&s, r.p, sizeof s);
        s = [self scissor:s];
        [e setScissorRect:s];
        if (tracing) fprintf(stderr, "trace:  scissor %lu %lu %lu %lu\n", (unsigned long)s.x, (unsigned long)s.y, (unsigned long)s.width, (unsigned long)s.height);
        break;
    }
    case 130: {
        MTLViewport v;
        memcpy(&v, r.p, sizeof v);
        [e setViewport:v];
        break;
    }
    case 122: { /* {ref, offset, stride}: within the buffer, and each a multiple of 4 (Metal: "offset(%lu) must be a
                   multiple of 4 bytes.", "instanceStride(%lu) must be a multiple of 4 bytes.") */
        id<MTLBuffer> b = [self get:U32(0) task:task kind:KIND_BUFFER];
        uint64_t off = U64(4), stride = U64(12);
        if ((b && (off >= b.length || off % 4)) || stride % 4) {
            [self complain:[NSString stringWithFormat:@"tessellation factors at %llu, %llu apart, of a %lu-byte buffer", off, stride, (unsigned long)b.length]];
            b = nil, stride = 0;
        }
        [e setTessellationFactorBuffer:b offset:b ? (NSUInteger)off : 0 instanceStride:(NSUInteger)stride];
        tess_factors = b != nil, tess_buf = b, tess_off = b ? off : 0, tess_stride = stride;
        break;
    }
    case 123: {
        float f;
        memcpy(&f, r.p, 4);
        [e setTessellationFactorScale:f];
        break;
    }
    case 104: [e setDepthStencilState:[self get:U32(0) task:task kind:KIND_DEPTH_STENCIL]]; break;
    case 107: [e setCullMode:(MTLCullMode)U32(0)]; break;
    case 108: {
        float f[3];
        memcpy(f, r.p, sizeof f);
        [e setDepthBias:f[0] slopeScale:f[1] clamp:f[2]];
        break;
    }
    case 109: [e setDepthClipMode:(MTLDepthClipMode)U32(0)]; break;
    case 115: [e setFrontFacingWinding:(MTLWinding)U32(0)]; break;
    case 124: [e setTriangleFillMode:(MTLTriangleFillMode)U32(0)]; break;
    case 119: [e setStencilFrontReferenceValue:U32(0) backReferenceValue:U32(4)]; break;
    case 118: { /* a header, then the rectangles (at most the encoder's: hv_pvg_command_problem) */
        uint32_t head = r.size % sizeof(MTLScissorRect), n = (r.size - head) / sizeof(MTLScissorRect);
        MTLScissorRect s[16];
        for (uint32_t i = 0; i < n && i < 16; i++) memcpy(&s[i], r.p + head + i * sizeof *s, sizeof *s), s[i] = [self scissor:s[i]];
        if (n && n <= 16) [e setScissorRects:s count:n];
        break;
    }
    case 131: {
        uint32_t head = r.size % sizeof(MTLViewport), n = (r.size - head) / sizeof(MTLViewport);
        if (n && n <= 16) [e setViewports:(const MTLViewport *)(r.p + head) count:n];
        break;
    }
    case 113: case 128: { /* samplers with LOD clamps: {index, count}, refs, minimums, maximums */
        uint32_t index = U32(0), count = U32(4);
        __unsafe_unretained id<MTLSamplerState> items[16];
        id strong[16];
        float lo[16], hi[16];
        for (uint32_t i = 0; i < count; i++) {
            strong[i] = [self get:U32(8 + i * 4) task:task kind:KIND_SAMPLER];
            items[i] = strong[i];
            memcpy(&lo[i], r.p + 8 + count * 4 + i * 4, 4);
            memcpy(&hi[i], r.p + 8 + count * 8 + i * 4, 4);
        }
        if (tag == 113) [e setFragmentSamplerStates:items lodMinClamps:lo lodMaxClamps:hi withRange:NSMakeRange(index, count)];
        else [e setVertexSamplerStates:items lodMinClamps:lo lodMaxClamps:hi withRange:NSMakeRange(index, count)];
        break;
    }
    case 135: case 137: { /* {count, usage}, or {count, usage 16, stages 16}, then the resources: buffers and textures */
        uint32_t count = U32(0);
        if (count > 256 || r.size < 8 + count * 4ull) break;
        __unsafe_unretained id<MTLResource> items[256];
        NSMutableArray *keep = [NSMutableArray new];
        uint32_t have = 0;
        for (uint32_t i = 0; i < count; i++) {
            HvObject *x = [self object:U32(8 + i * 4) task:task kinds:1u << KIND_BUFFER | 1u << KIND_TEXTURE];
            if (!x) continue;
            [keep addObject:x->object];
            items[have++] = x->object;
        }
        if (!have) break;
        if (tag == 135) [e useResources:items count:have usage:(MTLResourceUsage)U32(4)];
        else [e useResources:items count:have usage:(MTLResourceUsage)U16(4) stages:(MTLRenderStages)U16(6)];
        break;
    }
    case 101: {
        float c[4];
        memcpy(c, r.p, sizeof c);
        [e setBlendColorRed:c[0] green:c[1] blue:c[2] alpha:c[3]];
        break;
    }
    case 13: { /* {start 16, count 16, index ref, offset 16, instances 16, base instance 16, control points 16} */
        if (!tess_factors) break; /* Metal crashes without a factor buffer */
        id<MTLBuffer> b = [self get:U32(4) task:task kind:KIND_BUFFER];
        if (U16(14) && [self patchDraw:13 start:U16(0) count:U16(2) points:U16(14) patches:b at:U16(8) indexes:nil at:0] && [self factors:13 start:U16(0) count:U16(2) instances:U16(10) base:U16(12) indexed:b != nil])
            [e drawPatches:U16(14) patchStart:U16(0) patchCount:U16(2) patchIndexBuffer:b patchIndexBufferOffset:U16(8) instanceCount:U16(10) baseInstance:U16(12)];
        draws++;
        break;
    }
    case 12: { /* {start 64, count 64, index ref, offset 64, instances 64, base instance 64, control points 16} */
        if (!tess_factors) break;
        id<MTLBuffer> b = [self get:U32(16) task:task kind:KIND_BUFFER];
        if (U16(44) && [self patchDraw:12 start:U64(0) count:U64(8) points:U16(44) patches:b at:U64(20) indexes:nil at:0] && [self factors:12 start:U64(0) count:U64(8) instances:U64(28) base:U64(36) indexed:b != nil])
            [e drawPatches:U16(44) patchStart:(NSUInteger)U64(0) patchCount:(NSUInteger)U64(8) patchIndexBuffer:b patchIndexBufferOffset:(NSUInteger)U64(20)
             instanceCount:(NSUInteger)U64(28) baseInstance:(NSUInteger)U64(36)];
        draws++;
        break;
    }
    case 14: { /* {start 64, count 64, patch index ref, offset 64, control point index ref, offset 64, instances 64, base instance 64, control points 16} */
        if (!tess_factors) break;
        id<MTLBuffer> patches = [self get:U32(16) task:task kind:KIND_BUFFER], points = [self get:U32(28) task:task kind:KIND_BUFFER];
        if (points && U16(56) && [self patchDraw:14 start:U64(0) count:U64(8) points:U16(56) patches:patches at:U64(20) indexes:points at:U64(32)] &&
            [self factors:14 start:U64(0) count:U64(8) instances:U64(40) base:U64(48) indexed:patches != nil])
            [e drawIndexedPatches:U16(56) patchStart:(NSUInteger)U64(0) patchCount:(NSUInteger)U64(8) patchIndexBuffer:patches patchIndexBufferOffset:(NSUInteger)U64(20)
                  controlPointIndexBuffer:points controlPointIndexBufferOffset:(NSUInteger)U64(32) instanceCount:(NSUInteger)U64(40) baseInstance:(NSUInteger)U64(48)];
        draws++;
        break;
    }
    case 15: { /* {start 16, count 16, patch index ref, offset 16, control point index ref, offset 16, instances 16, base instance 16, control points 16} */
        if (!tess_factors) break;
        id<MTLBuffer> patches = [self get:U32(4) task:task kind:KIND_BUFFER], points = [self get:U32(10) task:task kind:KIND_BUFFER];
        if (points && U16(20) && [self patchDraw:15 start:U16(0) count:U16(2) points:U16(20) patches:patches at:U16(8) indexes:points at:U16(14)] &&
            [self factors:15 start:U16(0) count:U16(2) instances:U16(16) base:U16(18) indexed:patches != nil])
            [e drawIndexedPatches:U16(20) patchStart:U16(0) patchCount:U16(2) patchIndexBuffer:patches patchIndexBufferOffset:U16(8)
                  controlPointIndexBuffer:points controlPointIndexBufferOffset:U16(14) instanceCount:U16(16) baseInstance:U16(18)];
        draws++;
        break;
    }
    case 18: { /* {patch index ref, indirect ref, patch index offset 64, indirect offset 64, control points 16} */
        if (!tess_factors) break;
        id<MTLBuffer> patches = [self get:U32(0) task:task kind:KIND_BUFFER], indirect = [self get:U32(4) task:task kind:KIND_BUFFER];
        if (U16(24) && [self patchIndexes:patches offset:U64(8) size:4] && [self arguments:indirect offset:U64(16) size:16] &&
            [self indirect:18 src:indirect index:nil patches:patches points:nil])
            [e drawPatches:U16(24) patchIndexBuffer:patches patchIndexBufferOffset:(NSUInteger)U64(8) indirectBuffer:pass_args
                indirectBufferOffset:indirect_at * HV_PVG_CLAMP_STRIDE];
        draws++;
        break;
    }
    case 19: { /* {patch index ref, control point index ref, indirect ref, then their three offsets, 64, and control points 16} */
        if (!tess_factors) break;
        id<MTLBuffer> patches = [self get:U32(0) task:task kind:KIND_BUFFER], points = [self get:U32(4) task:task kind:KIND_BUFFER],
                      indirect = [self get:U32(8) task:task kind:KIND_BUFFER];
        if (points && U16(36) && [self patchIndexes:patches offset:U64(12) size:4] && [self patchIndexes:points offset:U64(20) size:pipeline_cpsize] &&
            [self arguments:indirect offset:U64(28) size:16] && [self indirect:19 src:indirect index:nil patches:patches points:points])
            [e drawIndexedPatches:U16(36) patchIndexBuffer:patches patchIndexBufferOffset:(NSUInteger)U64(12) controlPointIndexBuffer:points
                controlPointIndexBufferOffset:(NSUInteger)U64(20) indirectBuffer:pass_args indirectBufferOffset:indirect_at * HV_PVG_CLAMP_STRIDE];
        draws++;
        break;
    }
    case 1: /* {primitive, start 16, count 16} */
        [e drawPrimitives:(MTLPrimitiveType)U32(0) vertexStart:U16(4) vertexCount:U16(6)];
        draws++;
        break;
    case 3: /* {start, count, instances, primitive} */
        [e drawPrimitives:(MTLPrimitiveType)U16(6) vertexStart:U16(0) vertexCount:U16(2) instanceCount:U16(4)];
        draws++;
        break;
    case 7: { /* {primitive, index type, ref, count, offset} */
        id<MTLBuffer> b = [self get:U32(4) task:task kind:KIND_BUFFER];
        if ([self indexes:b type:U16(2) offset:U16(10) count:U16(8)])
            [e drawIndexedPrimitives:(MTLPrimitiveType)U16(0) indexCount:U16(8) indexType:(MTLIndexType)U16(2) indexBuffer:b indexBufferOffset:U16(10)];
        draws++;
        break;
    }
    case 6: {
        id<MTLBuffer> b = [self get:U32(4) task:task kind:KIND_BUFFER];
        if ([self indexes:b type:U16(2) offset:U64(16) count:U64(8)])
            [e drawIndexedPrimitives:(MTLPrimitiveType)U16(0) indexCount:(NSUInteger)U64(8) indexType:(MTLIndexType)U16(2) indexBuffer:b indexBufferOffset:(NSUInteger)U64(16)];
        draws++;
        break;
    }
    case 9: { /* {primitive, index type, ref, count 16, offset 16, instances 16} */
        id<MTLBuffer> b = [self get:U32(4) task:task kind:KIND_BUFFER];
        if ([self indexes:b type:U16(2) offset:U16(10) count:U16(8)])
            [e drawIndexedPrimitives:(MTLPrimitiveType)U16(0) indexCount:U16(8) indexType:(MTLIndexType)U16(2) indexBuffer:b indexBufferOffset:U16(10) instanceCount:U16(12)];
        draws++;
        break;
    }
    case 8: { /* {primitive, index type, ref, count 64, offset 64, instances 64} */
        id<MTLBuffer> b = [self get:U32(4) task:task kind:KIND_BUFFER];
        if ([self indexes:b type:U16(2) offset:U64(16) count:U64(8)])
            [e drawIndexedPrimitives:(MTLPrimitiveType)U16(0) indexCount:(NSUInteger)U64(8) indexType:(MTLIndexType)U16(2) indexBuffer:b indexBufferOffset:(NSUInteger)U64(16) instanceCount:(NSUInteger)U64(24)];
        draws++;
        break;
    }
    case 0: /* {primitive 32, start 64, count 64} */
        [e drawPrimitives:(MTLPrimitiveType)U32(0) vertexStart:(NSUInteger)U64(4) vertexCount:(NSUInteger)U64(12)];
        draws++;
        break;
    case 2: /* {start 64, count 64, instances 64, primitive} */
        [e drawPrimitives:(MTLPrimitiveType)U16(24) vertexStart:(NSUInteger)U64(0) vertexCount:(NSUInteger)U64(8) instanceCount:(NSUInteger)U64(16)];
        draws++;
        break;
    case 4: /* {start 64, count 64, instances 64, base instance 64, primitive} */
        [e drawPrimitives:(MTLPrimitiveType)U16(32) vertexStart:(NSUInteger)U64(0) vertexCount:(NSUInteger)U64(8) instanceCount:(NSUInteger)U64(16) baseInstance:(NSUInteger)U64(24)];
        draws++;
        break;
    case 5: /* {start, count, instances, base instance, primitive}, all 16 */
        [e drawPrimitives:(MTLPrimitiveType)U16(8) vertexStart:U16(0) vertexCount:U16(2) instanceCount:U16(4) baseInstance:U16(6)];
        draws++;
        break;
    case 10: case 28: { /* {primitive, index type, ref, offset 64, count 64, instances 64, base vertex 64, base instance 64} */
        id<MTLBuffer> b = [self get:U32(4) task:task kind:KIND_BUFFER];
        if ([self indexes:b type:U16(2) offset:U64(8) count:U64(16)])
            [e drawIndexedPrimitives:(MTLPrimitiveType)U16(0) indexCount:(NSUInteger)U64(16) indexType:(MTLIndexType)U16(2) indexBuffer:b indexBufferOffset:(NSUInteger)U64(8)
                          instanceCount:(NSUInteger)U64(24) baseVertex:(NSInteger)U64(32) baseInstance:(NSUInteger)U64(40)];
        draws++;
        break;
    }
    case 11: case 29: { /* {primitive, index type, ref, offset 16, count 16, instances 16, base vertex 16, base instance 16}; 29's base vertex can be negative */
        id<MTLBuffer> b = [self get:U32(4) task:task kind:KIND_BUFFER];
        NSInteger base_vertex = tag == 29 ? (NSInteger)(int16_t)U16(14) : (NSInteger)U16(14);
        if ([self indexes:b type:U16(2) offset:U16(8) count:U16(10)])
            [e drawIndexedPrimitives:(MTLPrimitiveType)U16(0) indexCount:U16(10) indexType:(MTLIndexType)U16(2) indexBuffer:b indexBufferOffset:U16(8)
                          instanceCount:U16(12) baseVertex:base_vertex baseInstance:U16(16)];
        draws++;
        break;
    }
    case 16: { /* {ref, primitive 16, 2 unused, offset 64} */
        id<MTLBuffer> b = [self get:U32(0) task:task kind:KIND_BUFFER];
        if ([self arguments:b offset:U64(8) size:16] && [self indirect:16 src:b index:nil patches:nil points:nil])
            [e drawPrimitives:(MTLPrimitiveType)U16(4) indirectBuffer:pass_args indirectBufferOffset:indirect_at * HV_PVG_CLAMP_STRIDE];
        draws++;
        break;
    }
    case 17: { /* {primitive, index type, index ref, indirect ref, index offset 64, indirect offset 64} */
        id<MTLBuffer> index = [self get:U32(4) task:task kind:KIND_BUFFER], indirect = [self get:U32(8) task:task kind:KIND_BUFFER];
        if ([self indexes:index type:U16(2) offset:U64(12) count:0] && [self arguments:indirect offset:U64(20) size:20] &&
            [self indirect:17 src:indirect index:index patches:nil points:nil])
            [e drawIndexedPrimitives:(MTLPrimitiveType)U16(0) indexType:(MTLIndexType)U16(2) indexBuffer:index indexBufferOffset:(NSUInteger)U64(12)
                      indirectBuffer:pass_args indirectBufferOffset:indirect_at * HV_PVG_CLAMP_STRIDE];
        draws++;
        break;
    }
    case 154: { /* {count, then a viewport and a render target offset for each view} */
        uint32_t count = U32(0);
        MTLVertexAmplificationViewMapping views[8];
        if (count == 0 || count > 8 || r.size < 4 + count * 8ull || ![e.device supportsVertexAmplificationCount:count]) break;
        for (uint32_t i = 0; i < count; i++)
            if (U32(8 + i * 8) >= (pass_layers ? pass_layers : 1)) { /* a layer the pass has (viewports: hv_pvg_command_problem) */
                [self complain:[NSString stringWithFormat:@"amplification to layer %u of a pass of %llu", U32(8 + i * 8), pass_layers]];
                return;
            }
        for (uint32_t i = 0; i < count; i++) {
            views[i].viewportArrayIndexOffset = U32(4 + i * 8);
            views[i].renderTargetArrayIndexOffset = U32(8 + i * 8);
        }
        [e setVertexAmplificationCount:count viewMappings:views];
        break;
    }
    case 155: case 162: { /* threads per tile [, then a tile region]: within a tile */
        MTLSize s;
        MTLRegion region;
        memcpy(&s, r.p, sizeof s);
        memcpy(&region, r.p + sizeof s, sizeof region);
        if (!s.width || !s.height || s.depth != 1 || s.width > e.tileWidth || s.height > e.tileHeight) {
            break;
        }
        if ((uint64_t)s.width * s.height > cur_render.maxTotalThreadsPerThreadgroup) {
            if (cur_render.threadgroupSizeMatchesTileSize) {
                [self complain:[NSString stringWithFormat:@"tile dispatch of %lux%lux%lu in a %lux%lu tile dropped: the pipeline takes %lu threads a tile (a threadgroup a tile); pass %llux%llu",
                                (unsigned long)s.width, (unsigned long)s.height, (unsigned long)s.depth, (unsigned long)e.tileWidth,
                                (unsigned long)e.tileHeight, (unsigned long)cur_render.maxTotalThreadsPerThreadgroup, pass_w, pass_h]];
                break;
            }
            // On iPad GPUs (like M1), the pipeline's max threads per threadgroup can be 448 or 640,
            // while macOS dispatches 32x32 (1024 threads). Instead of dropping the whole dispatch
            // (which causes missing menu bar backdrops and visual glitches), adaptively downscale s to fit.
            static int told_adapted;
            if (told_adapted < 8) {
                told_adapted++;
                [self complain:[NSString stringWithFormat:@"tile dispatch of %lux%lux%lu in a %lux%lu tile adapted to pipeline limit %lu; pass %llux%llu",
                                (unsigned long)s.width, (unsigned long)s.height, (unsigned long)s.depth, (unsigned long)e.tileWidth,
                                (unsigned long)e.tileHeight, (unsigned long)cur_render.maxTotalThreadsPerThreadgroup, pass_w, pass_h]];
            }
            NSUInteger maxThreads = cur_render.maxTotalThreadsPerThreadgroup;
            while ((uint64_t)s.width * s.height > maxThreads) {
                if (s.height >= s.width && s.height > 1) {
                    s.height /= 2;
                } else if (s.width > 1) {
                    s.width /= 2;
                } else {
                    break;
                }
            }
        }
        if (tag == 162 && (region.origin.z || region.size.depth != 1 || region.origin.x >= pass_w || region.origin.y >= pass_h ||
                           !region.size.width || !region.size.height)) {
            [self complain:[NSString stringWithFormat:@"tile dispatch in a region outside its pass dropped: %lu,%lu,%lu %lux%lux%lu in %llux%llu",
                            (unsigned long)region.origin.x, (unsigned long)region.origin.y, (unsigned long)region.origin.z,
                            (unsigned long)region.size.width, (unsigned long)region.size.height, (unsigned long)region.size.depth, pass_w, pass_h]];
            break;
        }
        if (tag == 162 && (region.size.width > pass_w - region.origin.x || region.size.height > pass_h - region.origin.y)) {
            /* A region that starts in the pass and runs past its edge (macOS rounds one up to whole tiles, and a pass is
               seldom whole tiles) was dropped whole, and what it draws went missing: the menu bar's backdrop among it.
               It is cut to the pass instead. Metal's debug layer takes any region and doesn't say if it is in tiles or
               pixels; the pass's pixels bound either, so the part inside is within both. */
            static int told; /* the first few with their sizes: every window size would be another line */
            if (told < 8) {
                told++;
                [self complain:[NSString stringWithFormat:@"tile dispatch in a region past its pass, cut to it: %lu,%lu %lux%lu in %llux%llu",
                                (unsigned long)region.origin.x, (unsigned long)region.origin.y, (unsigned long)region.size.width,
                                (unsigned long)region.size.height, pass_w, pass_h]];
            }
            if (region.size.width > pass_w - region.origin.x) region.size.width = (NSUInteger)(pass_w - region.origin.x);
            if (region.size.height > pass_h - region.origin.y) region.size.height = (NSUInteger)(pass_h - region.origin.y);
        }
        if (![self charge:hv_pvg_sat_mul(pass_w, pass_h)]) break; /* a thread a pixel, at most */
        static SEL sel_region;
        static dispatch_once_t once_region;
        dispatch_once(&once_region, ^{
            sel_region = sel_registerName("dispatchThreadsPerTile:inRegion:");
        });
        if (tag == 162 && [(id)e respondsToSelector:sel_region]) ((void (*)(id, SEL, MTLSize, MTLRegion))objc_msgSend)(e, sel_region, s, region);
        else [e dispatchThreadsPerTile:s];
        break;
    }
    case 164: { /* where to write the tile size: {buffer ref, offset} */
        uint8_t *out = guest_out(self, task, [self object:U32(0) task:task kind:KIND_BUFFER], U64(4), 4);
        if (out) {
            uint16_t dims[2] = {(uint16_t)e.tileWidth, (uint16_t)e.tileHeight};
            memcpy(out, dims, 4);
        }
        break;
    }
    default:
        [self complain:[NSString stringWithFormat:@"render command %u (%u bytes) skipped", tag, r.size]];
        break;
    }
}

/* Compute calls. Tags start at 200. */
- (void)compute:(id<MTLComputeCommandEncoder>)e tag:(uint32_t)tag rec:(Rec)r task:(uint32_t)task {
    if (!pipeline_set && (tag == 200 || tag == 202)) return;
    const char *why = NULL;
    switch (tag) {
    case 209: {
        MTLRegion region;
        if (r.size >= sizeof region) {
            memcpy(&region, r.p, sizeof region);
            [e setStageInRegion:region];
        }
        break;
    }
    case 214: /* barriers: encoders here are in order, and each exec is waited for */
    case 215:
        [e memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
        break;
    case 212: {
        id f = [self get:U32(0) task:task kind:KIND_FENCE];
        if (f) [e updateFence:f];
        break;
    }
    case 213: {
        id f = [self get:U32(0) task:task kind:KIND_FENCE];
        if (f) [e waitForFence:f];
        break;
    }
    case 208: {
        id state = [self get:U32(0) task:task kind:KIND_COMPUTE_PIPELINE];
        if (state) [e setComputePipelineState:state];
        pipeline_set = state != nil;
        cur_compute = state;
        break;
    }
    case 203: {
        __unsafe_unretained id<MTLBuffer> buffers[31];
        id strong[31];
        NSUInteger offsets[31];
        [self buffers:r task:task stride:12 into:buffers strong:strong offsets:offsets strides:NULL bound:bound[0]];
        [e setBuffers:buffers offsets:offsets withRange:NSMakeRange(U32(0), U32(4))];
        break;
    }
    case 207: if ([self offset:U64(4) of:bound[0] index:U32(0)]) [e setBufferOffset:(NSUInteger)U64(4) atIndex:U32(0)]; break;
    case 206: {
        uint32_t index = U32(0), count = U32(4);
        __unsafe_unretained id<MTLTexture> items[128];
        id strong[128];
        for (uint32_t i = 0; i < count; i++) {
            strong[i] = [self get:U32(8 + i * 4) task:task kind:KIND_TEXTURE];
            items[i] = strong[i];
        }
        [e setTextures:items withRange:NSMakeRange(index, count)];
        break;
    }
    case 204: {
        uint32_t index = U32(0), count = U32(4);
        __unsafe_unretained id<MTLSamplerState> items[16];
        id strong[16];
        for (uint32_t i = 0; i < count; i++) {
            strong[i] = [self get:U32(8 + i * 4) task:task kind:KIND_SAMPLER];
            items[i] = strong[i];
        }
        [e setSamplerStates:items withRange:NSMakeRange(index, count)];
        break;
    }
    case 211: [e setThreadgroupMemoryLength:(NSUInteger)U64(0) atIndex:U32(8)]; break;
    case 200: case 202: { /* threadgroups (or threads), then threads per threadgroup: none empty, and a group within the pipeline's */
        MTLSize a, b;
        memcpy(&a, r.p, sizeof a), memcpy(&b, r.p + sizeof a, sizeof b);
        uint64_t grid[3] = {a.width, a.height, a.depth}, group[3] = {b.width, b.height, b.depth};
        const char *bad = hv_pvg_grid_problem(tag == 202, grid, group, cur_compute.maxTotalThreadsPerThreadgroup);
        if (bad) {
            [self complain:[NSString stringWithFormat:@"dispatch of %lux%lux%lu groups of %lux%lux%lu: %s", (unsigned long)a.width, (unsigned long)a.height,
                            (unsigned long)a.depth, (unsigned long)b.width, (unsigned long)b.height, (unsigned long)b.depth, bad]];
            break;
        }
        if (tag == 200) [e dispatchThreadgroups:a threadsPerThreadgroup:b];
        else [e dispatchThreads:a threadsPerThreadgroup:b];
        break;
    }
    case 216: /* {width, height} of the imageblock, 32x32 from macOS 27's glass and blur kernels: without it their
                 imageblock memory is undefined, and toolbars came out black with coloured blocks */
        if (r.size < 8) break;
        if (U32(0) && U32(4) && U32(0) <= 256 && U32(4) <= 256 && !(U32(0) & (U32(0) - 1)) && !(U32(4) & (U32(4) - 1)))
            [e setImageblockWidth:U32(0) height:U32(4)];
        else why = "an imageblock size that isn't powers of 2 up to 256";
        break;
    case 219: /* not a command the framework knows either, though the guest sends it */
        break;
    default:
        [self complain:[NSString stringWithFormat:@"compute command %u (%u bytes) skipped", tag, r.size]];
        break;
    }
    if (why) [self complain:[NSString stringWithFormat:@"compute command %u dropped: %s", tag, why]];
}

/* Blit calls. Tags start at 300. Each copy's regions, offsets, slices and levels lie within what it copies between
   (pvg_command.h): past them Metal stops the app, or copies where nothing was meant to be. */
- (void)blit:(id<MTLBlitCommandEncoder>)e tag:(uint32_t)tag rec:(Rec)r task:(uint32_t)task {
    const char *why = NULL;
    switch (tag) {
    case 316: {
        id f = [self get:U32(0) task:task kind:KIND_FENCE];
        if (f) [e updateFence:f];
        break;
    }
    case 317: {
        id f = [self get:U32(0) task:task kind:KIND_FENCE];
        if (f) [e waitForFence:f];
        break;
    }
    case 300: { /* buffer to texture */
        id<MTLBuffer> b = [self get:U32(0) task:task kind:KIND_BUFFER];
        id<MTLTexture> t = [self get:U32(4) task:task kind:KIND_TEXTURE];
        if (!b || !t) break;
        MTLSize size;
        MTLOrigin origin;
        memcpy(&size, r.p + 0x20, sizeof size), memcpy(&origin, r.p + 0x38, sizeof origin);
        HvTexDesc d = made_desc(t);
        uint64_t o[3] = {origin.x, origin.y, origin.z}, s[3] = {size.width, size.height, size.depth};
        why = hv_pvg_copy_buffer_texture_problem(&d, t.isFramebufferOnly, U16(0x50), U16(0x52), o, s, b.length, U64(8), U64(0x10), U64(0x18), U32(0x54));
        if (!why)
            [e copyFromBuffer:b sourceOffset:(NSUInteger)U64(8) sourceBytesPerRow:(NSUInteger)U64(0x10) sourceBytesPerImage:(NSUInteger)U64(0x18) sourceSize:size
                    toTexture:t destinationSlice:U16(0x50) destinationLevel:U16(0x52) destinationOrigin:origin options:(MTLBlitOption)U32(0x54)];
        break;
    }
    case 301: { /* buffer to buffer */
        id<MTLBuffer> a = [self get:U32(0) task:task kind:KIND_BUFFER], b = [self get:U32(4) task:task kind:KIND_BUFFER];
        if (!a || !b) break;
        uint64_t size = U64(0x18);
        if (!hv_pvg_range_ok(a.length, U64(8), size) || !hv_pvg_range_ok(b.length, U64(0x10), size)) why = "past the end of a buffer";
        else [e copyFromBuffer:a sourceOffset:(NSUInteger)U64(8) toBuffer:b destinationOffset:(NSUInteger)U64(0x10) size:(NSUInteger)size];
        break;
    }
    case 302: { /* texture to buffer */
        id<MTLTexture> t = [self get:U32(0) task:task kind:KIND_TEXTURE];
        id<MTLBuffer> b = [self get:U32(4) task:task kind:KIND_BUFFER];
        if (!b || !t) break;
        MTLOrigin origin;
        MTLSize size;
        memcpy(&origin, r.p + 8, sizeof origin), memcpy(&size, r.p + 0x20, sizeof size);
        HvTexDesc d = made_desc(t);
        uint64_t o[3] = {origin.x, origin.y, origin.z}, s[3] = {size.width, size.height, size.depth};
        why = hv_pvg_copy_buffer_texture_problem(&d, t.isFramebufferOnly, U16(0x50), U16(0x52), o, s, b.length, U64(0x38), U64(0x40), U64(0x48), U16(0x54));
        if (!why)
            [e copyFromTexture:t sourceSlice:U16(0x50) sourceLevel:U16(0x52) sourceOrigin:origin sourceSize:size toBuffer:b
                destinationOffset:(NSUInteger)U64(0x38) destinationBytesPerRow:(NSUInteger)U64(0x40) destinationBytesPerImage:(NSUInteger)U64(0x48) options:(MTLBlitOption)U16(0x54)];
        break;
    }
    case 303: { /* texture to texture */
        id<MTLTexture> a = [self get:U32(0) task:task kind:KIND_TEXTURE], b = [self get:U32(4) task:task kind:KIND_TEXTURE];
        if (!a || !b) break;
        MTLOrigin from, to;
        MTLSize size;
        memcpy(&from, r.p + 8, sizeof from), memcpy(&size, r.p + 0x20, sizeof size), memcpy(&to, r.p + 0x38, sizeof to);
        HvTexDesc da = made_desc(a), db = made_desc(b);
        uint64_t f[3] = {from.x, from.y, from.z}, t[3] = {to.x, to.y, to.z}, s[3] = {size.width, size.height, size.depth};
        why = hv_pvg_copy_texture_problem(&da, a.isFramebufferOnly, U16(0x50), U16(0x52), f, &db, b.isFramebufferOnly, U16(0x54), U16(0x56), t, s, a == b);
        if (!why)
            [e copyFromTexture:a sourceSlice:U16(0x50) sourceLevel:U16(0x52) sourceOrigin:from sourceSize:size
                     toTexture:b destinationSlice:U16(0x54) destinationLevel:U16(0x56) destinationOrigin:to];
        break;
    }
    case 306: { /* fill: {ref, location, length, value} */
        id<MTLBuffer> b = [self get:U32(0) task:task kind:KIND_BUFFER];
        if (!b) break;
        if (!hv_pvg_range_ok(b.length, U64(4), U64(12))) why = "past the end of the buffer";
        else [e fillBuffer:b range:NSMakeRange((NSUInteger)U64(4), (NSUInteger)U64(12)) value:r.p[20]];
        break;
    }
    case 307: { /* mipmaps: of a colour texture the GPU can filter and render into */
        id<MTLTexture> t = [self get:U32(0) task:task kind:KIND_TEXTURE];
        if (!t) break;
        HvTexDesc d = made_desc(t);
        if (d.mips < 2 || !hv_pvg_color_format(d.format) || !hv_pvg_filterable(d.format, caps) || d.samples != 1 || !hv_pvg_blittable(&d, t.isFramebufferOnly) || d.type == HV_TEX_BUFFER)
            why = "mipmaps of a texture that can't have them";
        else if ([self charge:hv_pvg_sat_mul(hv_pvg_sat_mul((uint64_t)d.width * d.height, d.depth ? d.depth : 1), (d.array ? d.array : 1) * (d.type == 5 || d.type == 6 ? 12ull : 2ull))])
            [e generateMipmapsForTexture:t]; /* each level reads the last's pixels: all of them, at most twice the first's */
        break;
    }
    case 308: case 309: case 310: case 311: case 314: case 315: /* hints, and syncing memory Apple GPUs share */
        break;
    default:
        [self complain:[NSString stringWithFormat:@"blit command %u (%u bytes) skipped", tag, r.size]];
        break;
    }
    if (why) [self complain:[NSString stringWithFormat:@"blit command %u dropped: %s", tag, why]];
}

/* An info answer that holds a host address or resource ID, kept for a restore in another process (where
   each is different): the question, where the answer went and the bytes written there. */
typedef struct {
    uint32_t tag, ref, bufref, rec_size, written;
    uint32_t ref_entry[3], buf_entry[3];
    uint64_t off;
    uint8_t old[16];
} SnapAnswer; /* then the question's bytes */
#define SNAP_MAX_ANSWERS (1u << 16)
#define SNAP_ANSWER_REC_MAX 64u

/* What info reads of the question: {ref, buffer ref, offset}, with a kind first for 460 */
static uint32_t info_rec_min(uint32_t tag) { return tag == 460 ? 20 : 16; }

static BOOL info_is_address(uint32_t tag) {
    return tag == 460 || tag == 461 || tag == 462 || tag == 463 || tag == 464 || tag == 466 || tag == 467 || tag == 468;
}

/* The bytes an address answer is: an address (460, 461, 463), or a resource ID and its index */
static uint32_t info_null(uint32_t tag) { return tag == 460 || tag == 461 || tag == 463 ? 8 : 16; }

/* The question's {ref, buffer ref, offset}, as info reads them (rec at least info_rec_min(tag) bytes). */
static void info_question(uint32_t tag, const uint8_t *rec, uint32_t *ref, uint32_t *bufref, uint64_t *off) {
    memcpy(ref, rec, 4);
    memcpy(bufref, rec + (tag == 460 ? 8 : 4), 4);
    memcpy(off, rec + (tag == 460 ? 12 : 8), 8);
}

/* The object an info question is about: the kinds each tag answers for (anything for an identifier) */
static uint32_t info_kinds(uint32_t tag) {
    switch (tag) {
    case 460: return ~0u;
    case 457: case 466: return 1u << KIND_RENDER_PIPELINE | 1u << KIND_TILE_PIPELINE;
    case 450: case 467: return 1u << KIND_COMPUTE_PIPELINE;
    case 463: return 1u << KIND_HEAP;
    case 461: return 1u << KIND_BUFFER;
    case 462: return 1u << KIND_TEXTURE;
    case 464: return 1u << KIND_SAMPLER;
    case 468: return 1u << KIND_DEPTH_STENCIL;
    default: return 0;
    }
}

/* What the guest asks about objects, answered in its own buffers: tags start at
   450, {ref, buffer ref, offset}. The guest's Metal puts these real host values
   into its argument buffers. Each answer goes within a buffer the guest named. */
- (void)info:(uint32_t)tag rec:(Rec)r task:(uint32_t)task {
    uint32_t ref = U32(0), bufref = U32(4);
    uint64_t off = U64(8);
    if (tag == 451 || tag == 469) { /* how big a texture would be in a heap: {description, buffer ref, offset} */
        uint32_t at = tag == 451 ? 0x20 : 0x28;
        if (r.size < at + 12) return;
        MTLTextureDescriptor *t = tag == 451 ? descriptor_v1(r.p) : descriptor_v2(r.p);
        uint8_t *out = guest_out(self, task, [self object:U32(at) task:task kind:KIND_BUFFER], U64(at + 4), 16);
        if (!out) { [self complain:@"heap size query: no buffer"]; return; }
        if ([self refuse:t what:@"heap size query"]) return; /* Metal stops the app on a description it can't make */
        MTLSizeAndAlign sa = [device heapTextureSizeAndAlignWithDescriptor:t];
        uint64_t v[2] = {sa.size, sa.align};
        memcpy(out, v, 16);
        return;
    }
    if (tag == 460) { /* an object's identifier: {ref, kind, buffer ref, offset} */
        bufref = U32(8);
        off = U64(12);
    }
    uint32_t kinds = info_kinds(tag);
    if (!kinds) {
        [self complain:[NSString stringWithFormat:@"info command %u (%u bytes) skipped", tag, r.size]];
        return;
    }
    HvObject *to = [self object:bufref task:task kind:KIND_BUFFER];
    uint8_t *out = guest_out(self, task, to, off, 28);
    HvObject *oo = [self object:ref task:task kinds:kinds];
    id obj = oo ? oo->object : nil;
    if (!out || !obj) {
        [self complain:[NSString stringWithFormat:@"info %u: ref %u buffer %u (%s, %llu bytes) offset %llu: %s", tag, ref, bufref,
                        !to ? "unknown" : to->mapped ? "mapped" : "not mapped", to ? to->length : 0, off, out ? "no object" : "no buffer"]];
        /* No object (refused, or not one of the kinds asked about): a null address or ID, as the guest's own nil
           binding writes, so its argument buffer holds nothing stale from an earlier object or process to follow.
           Kept like any answer, so a restore that can make the object tells the guest its real one (snap_reanswer). */
        if (out && info_is_address(tag)) [self infoAnswer:tag rec:r task:task ref:ref bufref:bufref buffer:to offset:off out:out written:info_null(tag)];
        return;
    }
    SEL index_sel = sel_registerName("resourceIndex");
    uint64_t index = [obj respondsToSelector:index_sel] ? ((uint64_t (*)(id, SEL))objc_msgSend)(obj, index_sel) : 0;
    uint32_t written = 0;
    switch (tag) {
    case 460: {
        SEL sel = sel_registerName("uniqueIdentifier");
        uint64_t v = [obj respondsToSelector:sel] ? ((uint64_t (*)(id, SEL))objc_msgSend)(obj, sel) : (uint64_t)(uintptr_t)obj;
        memcpy(out, &v, 8);
        written = 8;
        break;
    }
    case 457: { /* render pipeline: {max threads, imageblock length, tile match, indirect} */
        id<MTLRenderPipelineState> p = obj;
        uint32_t v[2] = {(uint32_t)p.maxTotalThreadsPerThreadgroup, (uint32_t)p.imageblockSampleLength};
        memcpy(out, v, 8);
        out[8] = p.threadgroupSizeMatchesTileSize, out[9] = p.supportIndirectCommandBuffers;
        break;
    }
    case 450: { /* compute pipeline */
        id<MTLComputePipelineState> p = obj;
        uint64_t v[3] = {p.maxTotalThreadsPerThreadgroup, p.threadExecutionWidth, p.staticThreadgroupMemoryLength};
        memcpy(out, v, 24);
        out[24] = p.supportIndirectCommandBuffers;
        break;
    }
    case 463: { /* heap: where the whole of it is */
        uint64_t a = oo->heap_whole ? oo->heap_whole.gpuAddress : 0;
        memcpy(out, &a, 8);
        written = 8;
        break;
    }
    case 461: { /* buffer */
        uint64_t a = [(id<MTLBuffer>)obj gpuAddress];
        memcpy(out, &a, 8);
        written = 8;
        break;
    }
    case 462: case 464: case 466: case 467: case 468: { /* texture, sampler, render, compute, depth-stencil */
        if (![obj respondsToSelector:@selector(gpuResourceID)]) { /* a sampler or state made without argument buffer support */
            [self complain:[NSString stringWithFormat:@"info %u: ref %u has no resource ID", tag, ref]];
            written = info_null(tag), memset(out, 0, written); /* none to give: a null one, not what was there */
            break;
        }
        MTLResourceID id_ = ((MTLResourceID (*)(id, SEL))objc_msgSend)(obj, @selector(gpuResourceID));
        memcpy(out, &id_, 8);
        memcpy(out + 8, &index, 8);
        written = 16;
        break;
    }
    }
    if (written) [self infoAnswer:tag rec:r task:task ref:ref bufref:bufref buffer:to offset:off out:NULL written:written];
}

/* An address answer given (out: written as 0 here first, for an object there is none of), and kept for a restore */
- (void)infoAnswer:(uint32_t)tag rec:(Rec)r task:(uint32_t)task ref:(uint32_t)ref bufref:(uint32_t)bufref buffer:(HvObject *)to
            offset:(uint64_t)off out:(uint8_t *)out written:(uint32_t)written {
    if (out) memset(out, 0, written);
    if (!info_is_address(tag) || r.size < info_rec_min(tag)) return;
    if (r.size <= SNAP_ANSWER_REC_MAX) [self keepAnswer:tag rec:r task:task ref:ref bufref:bufref buffer:to offset:off written:written];
    else if (!answers_lost) { /* not kept, so not told again after a restore: saving would be wrong */
        fprintf(stderr, "hengevm: pvg-metal: info %u asked in %u bytes, more than an answer kept takes: this Mac can't be suspended from now on\n", tag, r.size);
        answers_lost = YES;
    }
}

/* Answers whose objects (or the buffer they went into) are gone, or are other objects now, let go: a restore
   would find them stale anyway. At the cap only: it reads every one's entries from the guest. */
- (void)pruneAnswers {
    uint32_t before = answers_count;
    for (NSNumber *task in answers.allKeys) {
        NSMutableDictionary<NSData *, NSData *> *mine = answers[task];
        NSMutableArray<NSData *> *gone = [NSMutableArray new];
        [mine enumerateKeysAndObjectsUsingBlock:^(NSData *key, NSData *v, BOOL *stop) {
            (void)stop;
            const SnapAnswer *a = v.bytes;
            if (!snap_entry_is(self, task.unsignedIntValue, a->ref, a->ref_entry) ||
                !snap_entry_is(self, task.unsignedIntValue, a->bufref, a->buf_entry))
                [gone addObject:key];
        }];
        [mine removeObjectsForKeys:gone];
        answers_count -= (uint32_t)gone.count;
        if (!mine.count) [answers removeObjectForKey:task];
    }
    fprintf(stderr, "hengevm: pvg-metal: object answers: %u kept, %u let go (their objects are gone)\n", answers_count, before - answers_count);
}

- (void)keepAnswer:(uint32_t)tag rec:(Rec)r task:(uint32_t)task ref:(uint32_t)ref bufref:(uint32_t)bufref buffer:(HvObject *)to offset:(uint64_t)off written:(uint32_t)written {
    if (answers_lost) return; /* a save is refused from now on: nothing to keep them for */
    HvObject *ro = [self object:ref task:task]; /* known: it was just answered for */
    if (!ro) return;
    NSMutableDictionary<NSData *, NSData *> *mine = answers[@(task)];
    if (!mine) answers[@(task)] = mine = [NSMutableDictionary new];
    struct { uint32_t bufref; uint64_t off; } __attribute__((packed)) k = {bufref, off};
    NSData *key = [NSData dataWithBytes:&k length:sizeof k];
    BOOL fresh = mine[key] == nil;
    /* Pruned once at the cap; if that frees too little to last, the next answers would each prune again */
    if (fresh && answers_count >= SNAP_MAX_ANSWERS) {
        [self pruneAnswers];
        if (answers_count > SNAP_MAX_ANSWERS - SNAP_MAX_ANSWERS / 8) {
            fprintf(stderr, "hengevm: pvg-metal: over %u object answers in use: this Mac can't be suspended from now on (pausing still works)\n",
                    SNAP_MAX_ANSWERS - SNAP_MAX_ANSWERS / 8);
            answers_lost = YES;
            [answers removeAllObjects]; /* of no use now */
            answers_count = 0;
            return;
        }
    }
    NSMutableData *v = [NSMutableData dataWithLength:sizeof(SnapAnswer) + r.size];
    SnapAnswer *a = v.mutableBytes;
    a->tag = tag, a->ref = ref, a->bufref = bufref, a->rec_size = r.size, a->written = written, a->off = off;
    memcpy(a->ref_entry, ro->entry, sizeof a->ref_entry);
    memcpy(a->buf_entry, to->entry, sizeof a->buf_entry);
    memcpy(a->old, to->mapped + off, written);
    memcpy((uint8_t *)(a + 1), r.p, r.size);
    mine[key] = v;
    if (fresh) answers_count++;
}

/* HV_PVG_ENCODE_ONLY=1, for tests that send made-up work: everything up to and including encoding (where Metal's
   debug layer checks), and the command buffers are never committed. Their stamps are given back as if done. Only the
   tests' build (HV_PVG_TEST_HOOKS, from the Makefile) encodes and drops. Any other build fails closed: set there (to
   anything but empty or 0) it can only be someone meaning to keep made-up work off the GPU, so none of the guest's
   GPU work is run at all, as after the breaker trips (gpuFailed:), and nothing re-arms it; told once. */
enum { GUEST_WORK_RUNS, GUEST_WORK_ENCODED, GUEST_WORK_REFUSED };
static int guest_work(void) {
    static dispatch_once_t once;
    static int mode;
    dispatch_once(&once, ^{
        const char *e = getenv("HV_PVG_ENCODE_ONLY");
        if (!e || !*e || !strcmp(e, "0")) return;
#ifdef HV_PVG_TEST_HOOKS
        mode = GUEST_WORK_ENCODED;
#else
        mode = GUEST_WORK_REFUSED;
        fprintf(stderr, "hengevm: pvg-metal: HV_PVG_ENCODE_ONLY is set, and only the tests' build takes it: none of the guest's "
                        "graphics are run\n");
#endif
    });
    return mode;
}
static BOOL encode_only(void) { return guest_work() != GUEST_WORK_RUNS; } /* nothing of the guest's committed, either way */

/* A command buffer of the guest's work came back failed. A guest that hangs or faults the GPU once (the GPU's watchdog
   then restarts it, and everything the host draws stalls meanwhile) must not be able to do it again and again, so:
   - Timeout (2), PageFault (3) and StackOverflow (12) are the guest's work itself going wrong: they trip the breaker
     at once, and none of the guest's GPU work runs from then on.
   - Internal, AccessRevoked, OutOfMemory, InvalidResource, Memoryless, DeviceRemoved and any other code can come from
     the host too (memory pressure, a driver hiccup, the app losing the GPU for a moment): one alone is let go, and
     HV_GPU_STRIKES of them within HV_GPU_STRIKE_WINDOW trip it. A guest that causes them causes them with every
     frame, so it trips within a few frames; the host's own one-offs never come three in ten seconds.
   - NotPermitted (7) is never the guest's: an iPad app's GPU work refused while it is in the background.
   A restart of the guest's driver does not undo it, for the guest could restart it each time; the app can
   (hv_pvg_metal_gpu_rearm), and so does Suspend and Resume (see gpu_faulted). */
#define HV_GPU_STRIKES 3
#define HV_GPU_STRIKE_WINDOW (10ull * 1000000000ull)
- (void)gpuFailed:(NSError *)error {
    atomic_fetch_add(&failed, 1);
    fprintf(stderr, "hengevm: pvg-metal: command buffer: %s\n", error.localizedDescription.UTF8String);
    BOOL metal = [error.domain isEqualToString:MTLCommandBufferErrorDomain];
    long code = error.code;
    if (metal && code == MTLCommandBufferErrorNotPermitted) return;
    BOOL at_once = metal && (code == MTLCommandBufferErrorTimeout || code == MTLCommandBufferErrorPageFault ||
                             code == MTLCommandBufferErrorStackOverflow);
    if (!at_once) {
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        os_unfair_lock_lock(&gpu_strike_lock);
        gpu_strikes[gpu_strike_next++ % HV_GPU_STRIKES] = now;
        uint32_t recent = 0;
        for (int i = 0; i < HV_GPU_STRIKES; i++)
            if (gpu_strikes[i] && now - gpu_strikes[i] <= HV_GPU_STRIKE_WINDOW) recent++;
        os_unfair_lock_unlock(&gpu_strike_lock);
        if (recent < HV_GPU_STRIKES) {
            fprintf(stderr, "hengevm: pvg-metal: GPU failure %u of %d within %llu s before the guest's graphics are stopped\n", recent,
                    HV_GPU_STRIKES, HV_GPU_STRIKE_WINDOW / 1000000000ull);
            return;
        }
    }
    if (atomic_exchange(&gpu_faulted, 1)) return;
    fprintf(stderr, "hengevm: pvg-metal: the GPU failed this Mac's work (%s, error %ld): none of its graphics are run from now on, "
                    "until it is started again\n", error.domain.UTF8String, code);
}

/* The app's way back from the breaker (say, the person chose to try again): the guest's work runs again. */
- (void)gpuRearm {
    os_unfair_lock_lock(&gpu_strike_lock);
    memset(gpu_strikes, 0, sizeof gpu_strikes);
    os_unfair_lock_unlock(&gpu_strike_lock);
    if (atomic_exchange(&gpu_faulted, 0)) fprintf(stderr, "hengevm: pvg-metal: the guest's graphics run again\n");
}

/* The person's "Try Again" after the breaker tripped (the app's banner; hv_pvg_metal_gpu_try_again). Safe for the
   device: while tripped, the guest's execs were taken and their stamps given back as for any other, and everything
   but running them on the GPU went on (objects made and freed, surfaces mapped, the guest's own memory written, and
   what each dropped exec says the guest wrote: exec:). What the GPU never ran is lost, the uploads a dropped or
   failed command buffer carried with it too, so every object the guest's memory has the newest copy of goes up again
   when next used (reupload); what the dropped work would have drawn is missing until macOS draws it again, which it
   does as soon as anything on its screen changes. Not automatic, and only HV_GPU_TRIES times in a run: the same
   work may hang the GPU again, and each hang stalls everything the host draws until the GPU's watchdog ends it (with
   up to HV_PVG_GUEST_INFLIGHT buffers behind it). Past that, only a restart of the Mac (a new run) clears it.
   Not while any of the guest's command buffers is still on the GPU (a hang its watchdog has yet to end): that would
   only queue more behind it. Only those (guest_on_gpu): while stopped, each exec the guest sends gives its stamp back
   from an empty buffer behind the presenter's frames (finishChannel:), so counting those too would keep it settling
   for as long as the guest draws. */
#define HV_GPU_TRIES 2
- (int)gpuState {
    if (guest_work() == GUEST_WORK_REFUSED) return HV_PVG_GPU_STOPPED_FOR_GOOD; /* this build runs none of it, ever */
    if (!atomic_load(&gpu_faulted)) return HV_PVG_GPU_RUNS;
    if (atomic_load(&gpu_tries) >= HV_GPU_TRIES) return HV_PVG_GPU_STOPPED_FOR_GOOD;
    return atomic_load(&guest_on_gpu) ? HV_PVG_GPU_STOPPED_SETTLING : HV_PVG_GPU_STOPPED;
}
- (int)gpuTryAgain {
    if ([self gpuState] != HV_PVG_GPU_STOPPED) return -1;
    int used = atomic_load(&gpu_tries);
    do {
        if (used >= HV_GPU_TRIES) return -1;
    } while (!atomic_compare_exchange_weak(&gpu_tries, &used, used + 1));
    if (!atomic_load(&gpu_faulted) || atomic_load(&guest_on_gpu)) { /* another Try Again meanwhile, run and stopped again */
        atomic_fetch_sub(&gpu_tries, 1);
        return -1;
    }
    fprintf(stderr, "hengevm: pvg-metal: the person asked for the Mac's graphics again (%d of %d times this run)\n", used + 1, HV_GPU_TRIES);
    atomic_store(&reupload, 1); /* before the re-arm: the next exec that runs sees it */
    [self gpuRearm];
    return 0;
}

/* How many of the guest's command buffers may be committed and not done. Each exec is one command buffer, and the
   GPU runs one queue's in order, so two keep it busy: one running, the next already committed (the worker encodes
   the one after meanwhile). The guest itself waits for stamps between the few batches of a frame (hv_pvg_lite_
   complete), so it seldom has more ready; and when execs are tiny, what limits them is the time from a buffer's end
   to its completion handler (a fraction of a millisecond), and 4 in flight still takes thousands a second, far past
   the dozens a frame macOS sends. A bigger number buys nothing, and costs this: once one of them hangs the GPU, the
   ones committed behind it run too (Metal can't take back a committed buffer) and each can hang until the watchdog
   stops it. With 4, a guest's hang costs at most 4 watchdog periods, then the breaker (gpuFailed:) stops the rest.
   The host's own buffers (the presenter, finishChannel's and afterGPU's empty ones) aren't counted: they can't hang. */
#define HV_PVG_GUEST_INFLIGHT 4
- (dispatch_semaphore_t)guestSlots {
    if (!guest_slots) { /* made at 0 and raised: dispatch stops a process that frees a semaphore below its start */
        guest_slots = dispatch_semaphore_create(0);
        for (int i = 0; i < HV_PVG_GUEST_INFLIGHT; i++) dispatch_semaphore_signal(guest_slots);
    }
    return guest_slots;
}

/* A completion that took past HV_PVG_LATE from its commit to the guest's stamp, told at most once a second, with where
   the time went: committed, just before the commit; gs and ge, the GPU's start and end (0 if not known, as for an
   empty buffer, whose GPU time then counts in "commit to GPU start"); called, when Metal ran the handler; started and finished, complete_q's part. All CACurrentMediaTime. */
#define HV_PVG_LATE 0.25
static void late_probe(HvReplay *r, const char *what, uint32_t ch, double committed, double gs, double ge, double called,
                       double started, double finished) {
    if (finished - committed < HV_PVG_LATE) return;
    int64_t now = (int64_t)finished, told = atomic_load(&r->late_told);
    if (now <= told || !atomic_compare_exchange_strong(&r->late_told, &told, now)) return;
    if (gs <= 0 || ge < gs) gs = ge = called;
    fprintf(stderr, "hengevm: pvg-metal: late completion (%s, channel %u): commit to GPU start %.2f s, GPU ran %.2f s, "
                    "GPU end to callback %.2f s, queued behind earlier guest-side work %.2f s, copy-back and stamp %.2f s\n",
            what, ch, gs - committed, ge - gs, called - ge, started - called, finished - started);
}

/* One exec: {task, resources, buffers}, {ref, flags} per resource, {offset, length} per buffer. */
/* Commits without waiting for the GPU, once one of the HV_PVG_GUEST_INFLIGHT slots is free: the next batch can be
   encoded while the GPU does this one. Waiting for a slot blocks only the worker, never a vCPU, the completion
   handlers (which free slots and never wait for the worker), the presenter or the mapper; Suspend's save gives up
   at its deadline if the worker is held here (hv_pvg_metal_save). A slot always comes free: every committed buffer
   completes, a hung one when the GPU's watchdog stops it. redraw: the work draws into the surface on screen, shown
   again once it is done (and only then: before, the presenter would show it half drawn). The video surfaces waiting
   to go back (writeback) go back on its completion, before the stamp; those it reads (readers) have it as their
   last_reader until then. */
- (BOOL)commit:(id<MTLCommandBuffer>)cb channel:(uint32_t)ch stamp:(uint32_t)stamp complete:(BOOL)complete redraw:(BOOL)redraw {
    static int sync = -1; /* HV_PVG_SYNC: wait for the GPU after every batch */
    if (sync < 0) sync = getenv("HV_PVG_SYNC") != NULL;
    HvPvgLite *l = lite;
    NSArray<HvObject *> *back = writeback.count ? [writeback copy] : nil;
    NSMutableArray<NSNumber *> *gens = back ? [NSMutableArray arrayWithCapacity:back.count] : nil; /* what each will have back */
    for (HvObject *s in back) [gens addObject:@(s->video->write_gen)];
    NSArray<HvVideo *> *read = readers;
    readers = nil;
    [writeback removeAllObjects];
    if (encode_only() || atomic_load(&gpu_faulted)) { /* never run: its stamp after what is in flight, as for no work */
        backs_none(back, gens);
        if (complete) [self finishChannel:ch stamp:stamp];
        return NO;
    }
    /* A slot: no time limit of its own, for each of the HV_PVG_GUEST_INFLIGHT in flight ends, done or stopped by the
       GPU's watchdog (seconds), and its completion gives its slot back either way. One that failed trips the breaker,
       and then this stops waiting at once (within the second), without one. */
    dispatch_semaphore_t slots = [self guestSlots];
    for (int waited = 0; dispatch_semaphore_wait(slots, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));) {
        if (atomic_load(&gpu_faulted)) {
            backs_none(back, gens);
            if (complete) [self finishChannel:ch stamp:stamp];
            return NO;
        }
        if (++waited == 5) fprintf(stderr, "hengevm: pvg-metal: the guest's GPU work has not finished in 5 s; waiting for it\n");
    }
    if (atomic_load(&gpu_faulted)) { /* tripped by what was in flight meanwhile */
        dispatch_semaphore_signal(slots);
        backs_none(back, gens);
        if (complete) [self finishChannel:ch stamp:stamp];
        return NO;
    }
    gpu_begun(self);
    [self closeTrail]; /* what wants to come after this can't come with an earlier empty buffer */
    for (HvVideo *v in read) { /* before the commit: its completion, which lets it go, comes after */
        os_unfair_lock_lock(&v->reader_lock);
        v->last_reader = cb;
        os_unfair_lock_unlock(&v->reader_lock);
    }
    for (HvObject *s in back) { /* to come back on its completion: none drawn into again until then (backs_wait) */
        os_unfair_lock_lock(&s->video->video_lock);
        s->video->back_cb = cb;
        os_unfair_lock_unlock(&s->video->video_lock);
        atomic_fetch_add(&s->video->backs_due, 1);
    }
    dispatch_queue_t cq = complete_q;
    dispatch_semaphore_t sync_done = sync ? dispatch_semaphore_create(0) : nil; /* HV_PVG_SYNC: the stamp given back too */
    double committed = CACurrentMediaTime();
    atomic_fetch_add(&guest_side, 1);
    atomic_fetch_add(&guest_on_gpu, 1);
    /* Metal's handler: what touches none of the guest's pages, the slot given back with it; the rest after the earlier
       completions' on complete_q, and only then is this one no longer in flight. */
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        double called = CACurrentMediaTime(), gs = done.GPUStartTime, ge = done.GPUEndTime;
        atomic_store_explicit(&self->st_gpu_since, stage_now(), memory_order_relaxed);
        for (HvVideo *v in read) { /* done: nothing to wait for, nor to keep (a planar one can outlive its mappings). Not
                                      behind video_lock (reader_lock) */
            os_unfair_lock_lock(&v->reader_lock);
            if (v->last_reader == done) v->last_reader = nil;
            os_unfair_lock_unlock(&v->reader_lock);
        }
        if (done.error || done.status == MTLCommandBufferStatusError) /* before the slot: the worker, once it has one, checks gpu_faulted */
            [self gpuFailed:done.error ?: [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal userInfo:nil]];
        else if (redraw) [self screenChanged];
        atomic_fetch_sub(&self->guest_on_gpu, 1); /* after gpuFailed: a failure is never seen as running, then settled */
        dispatch_semaphore_signal(slots);
        BOOL drawn = !done.error; /* what failed may be half drawn */
        dispatch_async(cq, ^{
            double started = CACurrentMediaTime();
            if (drawn) {
                /* all of them, before the stamp: nothing drew into them since (backs_wait) */
                for (NSUInteger i = 0; i < back.count; i++) video_back(self, back[i], gens[i].unsignedLongLongValue);
            } else
                backs_none(back, gens);
            for (HvObject *s in back) { /* made or none: no longer to come from here */
                os_unfair_lock_lock(&s->video->video_lock);
                if (s->video->back_cb == done) s->video->back_cb = nil;
                os_unfair_lock_unlock(&s->video->video_lock);
                atomic_fetch_sub(&s->video->backs_due, 1);
            }
            if (complete) hv_pvg_lite_complete(l, ch, stamp);
            late_probe(self, "exec", ch, committed, gs, ge, called, started, CACurrentMediaTime());
            atomic_fetch_sub(&self->guest_side, 1);
            atomic_fetch_sub(&self->inflight, 1);
            if (sync_done) dispatch_semaphore_signal(sync_done);
        });
    }];
    if (hv_frame_watching()) { /* the benchmark: how long it waits to be scheduled, which a present's hold makes longer */
        uint64_t queued_at = stage_now();
        [cb addScheduledHandler:^(id<MTLCommandBuffer> unused) {
            (void)unused;
            hv_frame_guest_waited(stage_now() - queued_at);
        }];
    }
    [cb commit];
    last_cb = cb;
    if (sync_done && dispatch_semaphore_wait(sync_done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)HV_PVG_GPU_WAIT))) {
        atomic_fetch_add(&gpu_waits_given_up, 1); /* HV_PVG_SYNC is for finding bugs: a hang is told, not waited out */
        [self complain:@"HV_PVG_SYNC: a batch still running after its longest wait: not waited for"];
    }
    return YES;
}

/* then, with the trailing empty buffer's list (trail), if there is one still to run and nothing of the guest's, nor of
   the presenter's if any_presents is NO, was committed after it: it runs on complete_q after that buffer is done, so
   after all the GPU's work before it. The worker's. */
- (BOOL)joinTrail:(void (^)(void))then presents:(uint64_t)p any:(BOOL)any_presents {
    os_unfair_lock_lock(&trail_lock);
    BOOL joined = trail_then && (any_presents || trail_presents == p);
    if (joined) [trail_then addObject:[then copy]];
    os_unfair_lock_unlock(&trail_lock);
    return joined;
}
/* A new trailing empty buffer (cb, made by the caller), committed, with then first on its list. It runs nothing: its
   completion comes after all the GPU's work before it, and its list then runs on complete_q, after the earlier
   completions' guest side (commit:). The worker's. */
- (void)startTrail:(id<MTLCommandBuffer>)cb then:(void (^)(void))then presents:(uint64_t)p what:(const char *)what channel:(uint32_t)ch {
    NSMutableArray *list = [NSMutableArray arrayWithObject:[then copy]];
    dispatch_queue_t cq = complete_q;
    double committed = CACurrentMediaTime();
    gpu_begun(self);
    atomic_fetch_add(&guest_side, 1);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        double called = CACurrentMediaTime(), gs = done.GPUStartTime, ge = done.GPUEndTime;
        atomic_store_explicit(&self->st_gpu_since, stage_now(), memory_order_relaxed);
        dispatch_async(cq, ^{
            double started = CACurrentMediaTime();
            os_unfair_lock_lock(&self->trail_lock); /* closed: nothing joins it from here on */
            if (self->trail_then == list) self->trail = nil, self->trail_then = nil;
            os_unfair_lock_unlock(&self->trail_lock);
            for (void (^each)(void) in list) each();
            late_probe(self, what, ch, committed, gs, ge, called, started, CACurrentMediaTime());
            atomic_fetch_sub(&self->guest_side, 1);
            atomic_fetch_sub(&self->inflight, 1);
        });
    }];
    os_unfair_lock_lock(&trail_lock);
    trail = cb, trail_then = list, trail_presents = p;
    os_unfair_lock_unlock(&trail_lock);
    [cb commit];
    last_cb = cb;
}
/* The guest's work committed after the trail (commit:): what wants to come after it can't join the trail from now on */
- (void)closeTrail {
    os_unfair_lock_lock(&trail_lock);
    trail = nil, trail_then = nil;
    os_unfair_lock_unlock(&trail_lock);
}

/* A command that used the GPU not at all, or waited for by others: its stamp after everything before it. */
- (void)finishChannel:(uint32_t)ch stamp:(uint32_t)stamp {
    if (atomic_load(&inflight) == 0 || encode_only()) {
        hv_pvg_lite_complete(lite, ch, stamp);
        return;
    }
    HvPvgLite *l = lite;
    void (^then)(void) = ^{ hv_pvg_lite_complete(l, ch, stamp); };
    if ([self joinTrail:then presents:0 any:YES]) return; /* only the guest's work matters to a stamp */
    [self startTrail:[queue commandBuffer] then:then presents:atomic_load(&presents) what:"stamp only" channel:ch];
}

/* A swap's stamp: it tells the guest the buffer swapped away from is free to draw into again, so it comes after the
   GPU's work so far, the presenter's frames that read that buffer too (presenter_take), not only the guest's work as
   finishChannel's. What a real display does: a buffer is let go once scan-out has moved off it. */
- (void)finishSwap:(uint32_t)ch stamp:(uint32_t)stamp {
    HvPvgLite *l = lite;
    [self afterGPU:^{ hv_pvg_lite_complete(l, ch, stamp); }];
}

/* then, once the GPU has done all the work handed to it so far (the exec's and the presenter's): at once if there is
   none, else with the trail (joinTrail:), else from the completion of a new empty buffer. Never waits, and never makes
   a command buffer under screen_lock: [queue commandBuffer] waits while the queue has 64, and the main thread takes
   screen_lock (hv_pvg_metal_set_layer, hide_pointer). */
- (void)afterGPU:(void (^)(void))then {
    for (id<MTLCommandBuffer> cb = nil;;) {
        os_unfair_lock_lock(&screen_lock); /* the presenter commits under it: none of its work is missed in between */
        if (presenter_drawing || presenter_running || screen_dirty) {
            if (!present_then) present_then = [NSMutableArray new];
            [present_then addObject:[then copy]];
            os_unfair_lock_unlock(&screen_lock);
            return;
        }
        if (atomic_load(&inflight) == 0 || encode_only()) {
            os_unfair_lock_unlock(&screen_lock);
            then(); /* cb, if made, is let go uncommitted */
            return;
        }
        uint64_t p = atomic_load(&presents);
        if ([self joinTrail:then presents:p any:NO]) {
            os_unfair_lock_unlock(&screen_lock);
            return;
        }
        if (cb) {
            [self startTrail:cb then:then presents:p what:"after the GPU" channel:0];
            os_unfair_lock_unlock(&screen_lock);
            return;
        }
        os_unfair_lock_unlock(&screen_lock);
        cb = [queue commandBuffer]; /* then looked at again: the presenter may have committed meanwhile */
    }
}

/* Records shorter than the furthest field a decoder reads (the render pass's 0x248) are read from a zeroed copy:
   the decoders read fixed offsets, never past a record into the next or past the command buffer. */
#define REC_PAD 0x250
static Rec rec_at(const uint8_t *p, uint32_t size, uint8_t pad[REC_PAD]) {
    if (size >= REC_PAD) return (Rec){p, size};
    memset(pad, 0, REC_PAD);
    memcpy(pad, p, size);
    return (Rec){pad, size};
}

- (BOOL)isObject:(HvObject *)o matchShown:(id<MTLTexture>)shown {
    if (!shown) return NO;
    for (; o; o = o->parent) {
        if (o->object == shown || o->scanout == shown) return YES;
        if (o->surface && (o->surface->scanout == shown || o->surface->object == shown)) return YES;
    }
    return NO;
}

/* Whether the object is the surface on screen, or made from it (a view, a texture over its buffer) */
- (BOOL)isShown:(HvObject *)o {
    os_unfair_lock_lock(&screen_lock);
    id<MTLTexture> shown = screen_tex;
    os_unfair_lock_unlock(&screen_lock);
    return [self isObject:o matchShown:shown];
}

/* This exec's work (hv_pvg_command_work's units), taken from its budget: NO once it is past HV_PVG_EXEC_WORK, and
   for the rest of the exec (told once). The exec is still committed and its stamp given back: the guest goes on,
   short of what it asked for, as after any command not done. */
- (BOOL)charge:(uint64_t)units {
    if (exec_over) return NO;
    if (units <= HV_PVG_EXEC_WORK - exec_work) {
        exec_work += units;
        return YES;
    }
    exec_over = YES;
    [self complain:@"an exec's work past its budget: the rest of the exec dropped"];
    return NO;
}

/* A command buffer's segment: its records, which encoder they are for, and whether they go on from the last segment
   (cont) and into the next (more): see exec: */
typedef struct {
    const uint8_t *p, *end;
    uint32_t type, cont, more;
} HvSeg;

#ifndef HV_PVG_SEG_DATA /* an exec's segments (the tests' can fail) */
#define HV_PVG_SEG_DATA(n) [[NSMutableData alloc] initWithLength:(n)]
#endif
#define HV_PVG_EXEC_COPY (64ull << 20) /* the bytes of its command buffers an exec may copy, all told (exec:) */
#define HV_PVG_PASS_INDIRECT 65536 /* a pass's indirect draws that are clamped (2 MB of arguments); the rest dropped */
#ifndef HV_PVG_EXEC_INDIRECT /* and an exec's, over all its passes (8 MB of arguments, 10 MB of jobs) */
#define HV_PVG_EXEC_INDIRECT 262144
#endif

static BOOL args_within(id<MTLBuffer> b, uint64_t off, uint64_t size) { return b && off % 4 == 0 && off <= b.length && b.length - off >= size; }
static uint32_t ids_from(id<MTLBuffer> b, uint64_t off, uint64_t size) { /* whole ids from off, at most 0xfffffffe */
    uint64_t n = (b.length - off) / size;
    return n > 0xfffffffeull ? 0xfffffffeu : (uint32_t)n;
}

/* Before a pass whose description record ends at p in segs[s]: its indirect draws (render 16 to 19), found just as
   exec: will go through them (to the next pass, or wherever the encoder ends), have their arguments copied into a
   buffer of the host's and clamped there by the GPU (HV_PVG_CLAMP_MSL), before the pass; the draws take only those.
   The copy waits for every fence the pass waits for, as the draws would have. */
- (void)clampPass:(const HvSeg *)segs count:(uint32_t)nseg from:(uint32_t)s at:(const uint8_t *)p task:(uint32_t)task
    commandBuffer:(id<MTLCommandBuffer>)cb {
    NSMutableArray<HvIndirect *> *found = nil;
    NSMutableOrderedSet *waits = nil; /* each once */
    uint32_t weight = 1, frec = 12, fstep = 3, cpsize = 4;
    id<MTLBuffer> factors = nil; /* as exec: will have them at each draw (render 116 and 122) */
    uint64_t foff = 0, fstride = 0;
    uint8_t pad[REC_PAD];
    for (;;) {
        const uint8_t *end = segs[s].end;
        BOOL next_pass = NO;
        while (end - p >= 8) {
            uint32_t tag, size;
            memcpy(&tag, p, 4), memcpy(&size, p + 4, 4);
            if (size < 8 || size > (uint64_t)(end - p)) break;
            const uint8_t *at = p;
            Rec r = rec_at(p + 8, size - 8, pad);
            p += size;
            if (tag == 26) { next_pass = YES; break; }
            if (tag == 116) {
                HvObject *po = [self object:U32(0) task:task kinds:1u << KIND_RENDER_PIPELINE | 1u << KIND_TILE_PIPELINE];
                weight = po && po->tess_weight ? po->tess_weight : 1;
                frec = po && po->patch_rec ? po->patch_rec : 12, fstep = po && po->patch_rec ? po->patch_step : 3;
                cpsize = po && po->patch_index == 2 ? 2 : 4;
            }
            if (tag == 122) {
                factors = [self get:U32(0) task:task kind:KIND_BUFFER], foff = U64(4), fstride = U64(12);
                if (factors && foff >= factors.length) factors = nil;
                if (!factors) foff = 0;
            }
            if (tag == 25) {
                id f = [self get:U32(0) task:task kind:KIND_FENCE];
                if (f) {
                    if (!waits) waits = [NSMutableOrderedSet new];
                    [waits addObject:f];
                }
            }
            if (tag < 16 || tag > 19) continue;
            if (found.count == HV_PVG_PASS_INDIRECT) {
                [self complain:@"a pass's indirect draws past the clamp's: the rest dropped"];
                continue;
            }
            if (exec_indirect >= HV_PVG_EXEC_INDIRECT) {
                [self complain:@"an exec's indirect draws past the clamp's: the rest dropped"];
                continue;
            }
            exec_indirect++;
            HvIndirect *d = [HvIndirect new];
            d->at = at, d->seg = s, d->tag = tag, d->weight = weight, d->kind = HV_CLAMP_NONE, d->limit = 0xffffffffu;
            if (tag == 16) { /* {ref, primitive 16, 2 unused, offset 64} */
                d->src = [self get:U32(0) task:task kind:KIND_BUFFER], d->src_off = U64(8);
                if (args_within(d->src, d->src_off, 16)) d->kind = HV_CLAMP_DRAW;
            } else if (tag == 17) { /* {primitive, index type, index ref, indirect ref, index offset 64, indirect offset 64} */
                d->index = [self get:U32(4) task:task kind:KIND_BUFFER], d->src = [self get:U32(8) task:task kind:KIND_BUFFER];
                d->src_off = U64(20);
                uint64_t isz = U16(2) ? 4 : 2, ioff = U64(12);
                if (U16(2) <= 1 && d->index && ioff % isz == 0 && ioff <= d->index.length && args_within(d->src, d->src_off, 20))
                    d->kind = HV_CLAMP_INDEXED, d->limit = ids_from(d->index, ioff, isz);
            } else { /* 18: {patch index ref, indirect ref, offsets 64: patch index, indirect}; 19: {patch index ref, control
                        point index ref, indirect ref, offsets 64: theirs, control points 16} */
                d->patches = [self get:U32(0) task:task kind:KIND_BUFFER];
                if (tag == 19) d->points = [self get:U32(4) task:task kind:KIND_BUFFER];
                d->src = [self get:U32(tag == 18 ? 4 : 8) task:task kind:KIND_BUFFER];
                uint64_t poff = U64(tag == 18 ? 8 : 12), coff = U64(20), cps = U16(36);
                d->src_off = U64(tag == 18 ? 16 : 28);
                BOOL ok = args_within(d->src, d->src_off, 16) && (!d->patches || (poff % 4 == 0 && poff < d->patches.length));
                if (tag == 19) ok = ok && d->points && cps && coff % cpsize == 0 && coff < d->points.length;
                d->factors = factors, d->foff = foff, d->fstride = fstride, d->frec = frec, d->fstep = fstep, d->cpsize = cpsize;
                if (ok && d->patches && (fstep == 1 || fstep == 3)) { /* the guest's patch ids: no factors cover them (hv_pvg_tess_fit) */
                    [self complain:[NSString stringWithFormat:@"indirect draw %u dropped: patch ids from the guest's index buffer, and "
                                                              @"tessellation factors for each patch", tag]];
                    ok = NO;
                }
                if (ok) {
                    d->kind = HV_CLAMP_PATCHES;
                    if (d->patches) d->limit = ids_from(d->patches, poff, 4);
                    if (tag == 19) { /* each patch id, cps control point indexes at id x cps (hv_pvg_patch_ids_problem): bound by
                                        place when the ids are the places; else only as far as any id reads cps of them */
                        uint32_t n = ids_from(d->points, coff, cpsize) / (uint32_t)cps;
                        if (!d->patches ? n < d->limit : !n) d->limit = n;
                    }
                }
            }
            if (!found) found = [NSMutableArray new];
            [found addObject:d];
        }
        if (next_pass || !segs[s].more) break;
        uint32_t t = s + 1; /* the segment the encoder goes on into: past info ones that go on */
        while (t < nseg && segs[t].type > 2 && segs[t].cont) t++;
        if (t >= nseg || !segs[t].cont || segs[t].type != 0) break;
        s = t, p = segs[t].p;
    }
    if (!found.count) return;
    if (!clamp_tried) {
        clamp_tried = YES;
        NSError *err = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:@HV_PVG_CLAMP_MSL options:nil error:&err];
        id<MTLFunction> fn = [lib newFunctionWithName:@"hv_clamp"];
        clamp_pipe = fn ? [device newComputePipelineStateWithFunction:fn error:&err] : nil;
        if (!clamp_pipe) fprintf(stderr, "hengevm: pvg-metal: the indirect draws' clamp didn't build (%s): they are dropped\n", err.localizedDescription.UTF8String);
    }
    if (!clamp_pipe) return; /* no slots: each draw is dropped, told */
    NSUInteger n = found.count;
    id<MTLBuffer> args = [device newBufferWithLength:n * HV_PVG_CLAMP_STRIDE options:MTLResourceStorageModePrivate];
    NSMutableData *jobs = [[NSMutableData alloc] initWithLength:n * sizeof(HvClampJob)];
    if (!exec_left) {
        exec_left = [device newBufferWithLength:8 options:MTLResourceStorageModeShared];
        *(uint64_t *)exec_left.contents = HV_PVG_EXEC_WORK;
    }
    if (!args || !jobs || !exec_left) {
        [self complain:@"no memory for a pass's indirect draws: they are dropped"];
        return;
    }
    HvClampJob *job = jobs.mutableBytes;
    for (NSUInteger i = 0; i < n; i++) {
        HvIndirect *d = found[i];
        job[i] = (HvClampJob){d->kind, d->weight, d->limit, 0, 0, 0, 0, 0};
        if (d->kind == HV_CLAMP_PATCHES) /* no factor buffer: no room, no patches */
            job[i].frec = d->frec, job[i].froom = d->factors.length - d->foff, job[i].fstride = d->fstride, job[i].indexed = d->patches != nil,
            job[i].step = d->fstep;
    }
    id<MTLBuffer> big = nil; /* most passes' jobs go with the dispatch (setBytes, up to 4 KB): no buffer of their own */
    if (jobs.length > 4096 && !(big = [device newBufferWithBytes:jobs.bytes length:jobs.length options:MTLResourceStorageModeShared])) {
        [self complain:@"no memory for a pass's indirect draws: they are dropped"];
        return;
    }
    id<MTLBlitCommandEncoder> g = [cb blitCommandEncoder];
    for (id f in waits) [g waitForFence:f]; /* those the pass updates too: work before it may write the arguments */
    for (NSUInteger i = 0; i < n; i++) {
        HvIndirect *d = found[i];
        if (d->kind != HV_CLAMP_NONE)
            [g copyFromBuffer:d->src sourceOffset:(NSUInteger)d->src_off toBuffer:args destinationOffset:i * HV_PVG_CLAMP_STRIDE
                         size:d->kind == HV_CLAMP_INDEXED ? 20 : 16];
    }
    [g endEncoding];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    uint32_t count = (uint32_t)n;
    [c setComputePipelineState:clamp_pipe];
    [c setBuffer:args offset:0 atIndex:0];
    if (big) [c setBuffer:big offset:0 atIndex:1];
    else [c setBytes:jobs.bytes length:jobs.length atIndex:1];
    [c setBytes:&count length:4 atIndex:2];
    [c setBuffer:exec_left offset:0 atIndex:3];
    [c dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [c endEncoding];
    pass_indirect = found, pass_args = args, clamp_jobs_last = jobs;
}

/* The open pass's clamped arguments for this indirect draw (the record exec: is at, indirect_at), if they are for
   these buffers (and tessellation factors) and a pipeline no heavier than the clamp's: else nil, and told */
- (HvIndirect *)indirect:(uint32_t)tag src:(id)src index:(id)index patches:(id)patches points:(id)points {
    HvIndirect *d = indirect_at < pass_indirect.count ? pass_indirect[indirect_at] : nil;
    if (d && d->tag == tag && d->kind != HV_CLAMP_NONE && d->src == src && d->index == index && d->patches == patches && d->points == points &&
        d->weight >= pipeline_tess &&
        (d->kind != HV_CLAMP_PATCHES || (d->factors == tess_buf && d->foff == tess_off && d->fstride == tess_stride &&
                                         d->frec == (pipeline_frec ? pipeline_frec : 12) && d->fstep == (pipeline_frec ? pipeline_fstep : 3) &&
                                         (tag != 19 || d->cpsize == pipeline_cpsize)))) {
        indirect_drawn++;
        return d;
    }
    [self complain:[NSString stringWithFormat:@"indirect draw %u dropped: %s", tag, d ? "not as its pass's clamp found it" : "not clamped"]];
    return nil;
}

/* A new encoder: nothing of the last one's state carries over */
- (void)encoderBegins {
    pipeline_set = pipeline_tile = tess_factors = NO;
    pipeline_tess = 1, pipeline_frec = 0, pipeline_fstep = 3, pipeline_cpsize = 4;
    tess_buf = nil, tess_off = tess_stride = 0;
    cur_render = nil, cur_compute = nil;
    pass_indirect = nil, pass_args = nil, indirect_next = 0;
    memset(bound, 0, sizeof bound);
}

/* Returns YES if the GPU's completion gives the stamp back. */
- (BOOL)exec:(const uint32_t *)a length:(uint32_t)len channel:(uint32_t)ch stamp:(uint32_t)stamp {
    if (atomic_load(&gpu_faulted)) { /* see gpuFailed: dropped, its stamp given back */
        [self complain:@"exec dropped: the GPU failed this Mac's work before"];
        /* but what it says the guest wrote (flag 1) is kept, for a Try Again: else an object the guest wrote meanwhile
           keeps the GPU's older copy for good. Only those already made: one made later is made from the guest's pages. */
        uint32_t nres = len >= 12 ? a[1] : 0, task = len >= 12 ? a[0] : 0;
        HvTaskObjects *objs = 12 + nres * 8ull <= len ? task_objects(self, task, NO) : nil;
        for (uint32_t i = 0; objs && i < nres; i++) {
            if (!(a[3 + i * 2 + 1] & 1)) continue;
            HvObject *o = objs_get(objs, a[3 + i * 2]);
            if (!o) continue;
            o->guest_stale = NO, o->host_valid = NO;
            if (o->surface && o->surface->video) video_guest_wrote(o->surface->video);
        }
        return NO;
    }
    if (atomic_exchange(&reupload, 0)) /* tried again (gpuTryAgain) */
        for (uint32_t task = 0; task < HV_TASKS; task++) {
            HvObject *o;
            for (uint32_t at = 0; (o = objs_next(tasks[task], &at, NULL)); at++)
                if (o->guest_backed && !o->guest_stale) o->host_valid = NO;
        }
    if (guest_work() == GUEST_WORK_REFUSED) { /* as tripped, for good (see encode_only) */
        [self complain:@"exec dropped: HV_PVG_ENCODE_ONLY is set in a build that runs the guest's work"];
        return NO;
    }
    if (len < 12) {
        [self complain:[NSString stringWithFormat:@"exec of %u bytes dropped", len]];
        return NO;
    }
    uint32_t task = a[0], nres = a[1], nbuf = a[2];
    if (12 + nres * 8ull + nbuf * 16ull > len) { /* never quietly: a lost job is a frame that never gets drawn */
        [self complain:[NSString stringWithFormat:@"exec of %u bytes dropped: task %u, %u resources, %u buffers", len, task, nres, nbuf]];
        return NO;
    }
    const uint32_t *res = a + 3;
    const uint8_t *bufs = (const uint8_t *)(res + nres * 2);
    tracing = hv_pvg_tracing() != 0;
    if (tracing) fprintf(stderr, "trace: exec task %u, %u resources, %u buffers\n", task, nres, nbuf);
    double t_begin = now_seconds();
    /* both kept to the end: segs points into them. Before anything is changed for the resources: dropped, it is as
       if the exec never came */
    __attribute__((objc_precise_lifetime)) NSMutableArray<NSData *> *copies = [NSMutableArray new];
    __attribute__((objc_precise_lifetime)) NSMutableData *seg_data = HV_PVG_SEG_DATA((nbuf ? nbuf : 1) * sizeof(HvSeg));
    /* Each resource's object, looked up once (gfx-cost 7): the later uses, its flag 2 and 4 and video, are the same
       object, the exec's resources as they were at its start. Strong: a ref named again while encoding may be given a
       new object, and the old one is still what this exec's resource is. */
    for (uint32_t i = 0; i < exec_found_n; i++) exec_found[i] = nil; /* an exec a throw cut short: let go now */
    exec_found_n = 0;
    if (nres > exec_found_cap) {
        HvObject *__strong *grown = (__strong HvObject **)calloc(nres, sizeof(HvObject *));
        if (grown) free((void *)exec_found), exec_found = grown, exec_found_cap = nres;
    }
    if (!seg_data || nres > exec_found_cap) {
        [self complain:@"no memory for an exec's command buffers: dropped"];
        return NO;
    }
    HvObject *__strong *found = exec_found;
    exec_found_n = nres;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = nil;
    BOOL sync_after = NO, redraw = NO;
    os_unfair_lock_lock(&screen_lock);
    id<MTLTexture> current_shown = screen_tex;
    os_unfair_lock_unlock(&screen_lock);
    NSMutableArray<HvVideo *> *video = nil; /* the video surfaces this exec uses: it is their newest reader */
    NSMutableArray<HvObject *> *video_by = nil; /* a mapping of each that it names */
    NSMutableData *video_flags = nil; /* and all the flags it names it with, through any of them (uint32_t each) */
    for (uint32_t i = 0; i < nres; i++) {
        HvObject *o = found[i] = [self object:res[i * 2] task:task];
        if (!o) continue;
        uint32_t flags = res[i * 2 + 1];
        if (o->mapped && !refresh_pages(self, task, o)) { found[i] = nil; continue; } /* gone: nothing through old pages */
        HvObject *s = o->surface && o->surface->video ? o->surface : nil;
        if (s) { /* shared by every mapping of the same guest memory: one shadow, one count; done once, below */
            if (!video) video = [NSMutableArray new], video_by = [NSMutableArray new], video_flags = [NSMutableData new];
            NSUInteger at = [video indexOfObjectIdenticalTo:s->video];
            if (at == NSNotFound) {
                at = video.count;
                [video addObject:s->video], [video_by addObject:s];
                [video_flags increaseLengthBy:4];
            }
            ((uint32_t *)video_flags.mutableBytes)[at] |= flags;
        }
        if (env.tally && (o->entry[0] & 0xff) == OBJ_MAPPED_TEXTURE) {
            static uint32_t seen[256];
            uint32_t f = flags & 0xff, key = res[i * 2] & 0xff;
            if (seen[key] != f + 1) {
                seen[key] = f + 1;
                fprintf(stderr, "pvgres task %u ref %u mapped flags %x\n", task, res[i * 2], flags);
            }
        }
        if (flags & 1) o->guest_stale = NO, o->host_valid = NO; /* the guest wrote it */
        if (o->guest_backed && !o->host_valid) {
            if (!o->guest_stale) [self page:o toGuest:NO task:task blit:&blit commandBuffer:cb];
            o->host_valid = YES;
        }
        if (flags & 4) sync_after = YES;
        if ((flags & 2) && !redraw && current_shown) redraw = [self isObject:o matchShown:current_shown];
    }
    /* Each video surface once, whatever mappings of it the exec names with what: the guest's writing up, and only then
       is the GPU's drawing the newer (a mapping written by the guest after one drawn into would otherwise have the
       drawing never come back) */
    for (NSUInteger i = 0; i < video.count; i++) {
        HvVideo *v = video[i];
        uint32_t flags = ((const uint32_t *)video_flags.bytes)[i];
        /* drawn into again: a copy-back of earlier drawing still to come copies the IOSurface as it is when it runs, so
           it runs first, not over this one's half drawn. The GPU's work may not even be done yet, so not just what is
           already on complete_q: every one counted (backs_due) */
        if ((flags & 2) && !backs_wait(self, v)) {
            atomic_fetch_add(&gpu_waits_given_up, 1);
            [self complain:@"a video surface's copy-back still to come after its longest wait: drawn into all the same"];
        }
        if (flags & 1) video_guest_wrote(v); /* its rows go up */
        uint64_t uploads = video_uploads;
        sync_iosurface(self, video_by[i]);
        if (!(flags & 1)) video_unsaid += video_uploads - uploads;
        if (flags & 2) v->host_newer = YES, v->write_gen++; /* the GPU writes it: the guest's copy is the older */
    }
    [blit endEncoding];
    /* Bit 1: this exec writes it, so the guest's copy is out of date from here on. Never
       upload that over what the GPU drew: mostly zeros, which show as black boxes. */
    for (uint32_t i = 0; i < nres; i++)
        if (res[i * 2 + 1] & 2) { /* and what it is made from: a texture in a heap is the heap's bytes */
            for (HvObject *o = found[i]; o; o = o->parent) o->guest_stale = YES;
        }
    /* Each command buffer read first, into segments: a pass's indirect draws are clamped before it (clampPass:). Each
       copy only as long as its segment says, the same range named again its first copy (the guest's bytes as read
       then), and at most HV_PVG_EXEC_COPY bytes of them all an exec: they are kept to its end, so an exec naming a
       64 MB range a thousand times would otherwise hold 64 GB. */
    exec_work = 0, exec_indirect = 0, exec_over = NO, exec_left = nil;
    HvSeg *segs = seg_data.mutableBytes;
    uint32_t nseg = 0;
    uint64_t copied = 0;
    NSMutableDictionary<NSData *, NSNumber *> *seen = [NSMutableDictionary new]; /* {offset, length}: its segment */
    for (uint32_t b = 0; b < nbuf; b++) {
        uint64_t off, blen;
        memcpy(&off, bufs + b * 16, 8);
        memcpy(&blen, bufs + b * 16 + 8, 8);
        if (blen > (64u << 20) || blen < 8) continue;
        NSData *range = [NSData dataWithBytes:bufs + b * 16 length:16];
        NSNumber *again = seen[range];
        if (again) {
            segs[nseg++] = segs[again.unsignedIntValue];
            continue;
        }
        /* Its header first: how long the segment is, and so the copy */
        uint8_t head[24];
        uint32_t seglen, info, at = 8;
        if (hv_pvg_lite_task_read(lite, task, off, head, (uint32_t)MIN(blen, sizeof head))) { [self complain:@"command buffer unreadable"]; continue; }
        memcpy(&seglen, head, 4), memcpy(&info, head + 4, 4);
        if ((info & 0xff) == 5) { /* protection options, then the real header */
            if (blen < 24) continue;
            memcpy(&seglen, head + 16, 4), memcpy(&info, head + 20, 4);
            at = 24;
        }
        uint64_t want = at + MIN((uint64_t)seglen, blen - at); /* what is read and used: the header's values, not the copy's */
        if (want > HV_PVG_EXEC_COPY - copied) {
            [self complain:@"an exec's command buffers past what one may copy: the rest of them dropped"];
            break;
        }
        copied += want;
        NSMutableData *copy = [[NSMutableData alloc] initWithLength:(NSUInteger)want];
        if (!copy) {
            [self complain:@"no memory for an exec's command buffers: the rest of them dropped"];
            break;
        }
        if (hv_pvg_lite_task_read(lite, task, off, copy.mutableBytes, (uint32_t)want)) { [self complain:@"command buffer unreadable"]; continue; }
        [copies addObject:copy];
        const uint8_t *p = copy.bytes;
        seen[range] = @(nseg);
        segs[nseg++] = (HvSeg){p + at, p + want, info & 0xff, (info >> 8) & 0xff, (info >> 16) & 0xff};
    }
    id e = nil; /* the open encoder */
    uint32_t etype = 0;
    uint8_t pad[REC_PAD];
    for (uint32_t si = 0; si < nseg; si++) {
        const uint8_t *p = segs[si].p, *end = segs[si].end;
        uint32_t type = segs[si].type, cont = segs[si].cont, more = segs[si].more;
        if (!cont && e) { [e endEncoding]; e = nil; }
        if (type > 2) {
            const uint8_t *q = p;
            while (end - q >= 8) {
                uint32_t tag, sz;
                memcpy(&tag, q, 4), memcpy(&sz, q + 4, 4);
                if (sz < 8 || sz > (uint64_t)(end - q)) break;
                Rec r = rec_at(q + 8, sz - 8, pad);
                if (type != 4) [self complain:[NSString stringWithFormat:@"encoder type %u command %u (%u bytes) skipped", type, tag, sz]];
                else if (r.size < info_rec_min(tag)) [self complain:[NSString stringWithFormat:@"info %u of %u bytes skipped", tag, r.size]];
                else [self info:tag rec:r task:task];
                q += sz;
            }
            continue;
        }
        if (e && type != etype) { [e endEncoding]; e = nil; } /* one encoder's commands never go to another kind */
        if (!e && type == 1) e = [cb computeCommandEncoder], [self encoderBegins];
        if (!e && type == 2) e = [cb blitCommandEncoder];
        etype = type;
        @try {
            while (end - p >= 8) {
                uint32_t tag, size;
                memcpy(&tag, p, 4), memcpy(&size, p + 4, 4);
                if (size < 8 || size > (uint64_t)(end - p)) break;
                const uint8_t *at = p;
                Rec r = rec_at(p + 8, size - 8, pad);
                p += size;
                if (etype == 0 && tag >= 30 && tag <= 36) { /* these describe the pass that follows */
                    if (tag == 30 && r.size >= 4) pre_raster_samples = U32(0);
                    else if (tag == 34 && r.size >= 4) pre_imageblock = U32(0);
                    else if (tag == 35 && r.size >= 4) pre_threadgroup = U32(0);
                    else if (tag == 36 && r.size >= 4) pre_tile_w = U32(0) & 0xffff, pre_tile_h = U32(0) >> 16;
                    else if (tag == 33 || tag == 32) [self complain:[NSString stringWithFormat:@"pass description %u (%u bytes) not done", tag, size]];
                    continue;
                }
                if (etype == 0 && tag == 26) {
                    if (e) [e endEncoding];
                    e = nil;
                    [self encoderBegins];
                    if (exec_over) continue;
                    [self clampPass:segs count:nseg from:si at:p task:task commandBuffer:cb];
                    e = [self beginPass:r task:task commandBuffer:cb];
                    continue;
                }
                if (!e || exec_over) continue;
                if (etype == 0 && tag >= 16 && tag <= 19) { /* its clamped arguments: the pass's next found at this record */
                    indirect_at = UINT32_MAX;
                    for (uint32_t i = indirect_next; i < pass_indirect.count; i++)
                        if (pass_indirect[i]->at == at && pass_indirect[i]->seg == si) { indirect_at = i, indirect_next = i + 1; break; }
                }
                /* Each command's slots, lists and values within what Metal takes (pvg_command.h), else not done */
                const char *why = hv_pvg_command_problem(tag, r.p, r.size, &limits);
                if (why) {
                    [self complain:[NSString stringWithFormat:@"%s command %u (%u bytes) dropped: %s", etype == 0 ? "render" : etype == 1 ? "compute" : "blit",
                                    tag, r.size, why]];
                    continue;
                }
                /* and within the exec's budget: a patch draw's work is its pipeline's patches' too */
                uint64_t work = hv_pvg_command_work(tag, r.p, r.size);
                if (etype == 0 && tag >= 12 && tag <= 15) work = hv_pvg_sat_mul(work, pipeline_tess);
                if (work && ![self charge:work]) continue;
                if (etype == 0) [self render:e tag:tag rec:r task:task];
                else if (etype == 1) [self compute:e tag:tag rec:r task:task];
                else [self blit:e tag:tag rec:r task:task];
            }
        } @catch (NSException *x) {
            [self complain:[NSString stringWithFormat:@"exception: %@", x.reason]];
            atomic_fetch_add(&failed, 1);
        }
        if (!more && e) { [e endEncoding]; e = nil; }
    }
    if (e) [e endEncoding];
    pass_indirect = nil, pass_args = nil, exec_left = nil;
    if (sync_after) { /* the guest wants these back as soon as this is done */
        id<MTLBlitCommandEncoder> back = nil;
        for (uint32_t i = 0; i < nres; i++) {
            if (!(res[i * 2 + 1] & 4)) continue;
            HvObject *o = found[i];
            if (!o) continue;
            HvObject *s = o->surface;
            if (s && s->video && s->video->host_newer) back_once(writeback, s); /* on completion */
            if (!o->guest_backed) continue;
            [self page:o toGuest:YES task:task blit:&back commandBuffer:cb];
            o->guest_stale = NO;
        }
        [back endEncoding];
    }
    for (uint32_t i = 0; i < nres; i++) found[i] = nil;
    exec_found_n = 0;
    double t1 = now_seconds();
    readers = video; /* only work that runs is waited for (commit:) */
    [self commit:cb channel:ch stamp:stamp complete:YES redraw:redraw];
    double t2 = now_seconds();
    encode_time += t1 - t_begin, gpu_wait_time += t2 - t1;
    if (t1 - t_begin > encode_max) encode_max = t1 - t_begin;
    if (++execs % 2000 == 0) {
        static double t0;
        if (!t0) t0 = now_seconds();
        fprintf(stderr, "hengevm: pvg-metal: %llu execs, %llu draws, %llu failed, %llu synced, %llu remaps, %.0f s; encoding %.2f s (longest %.1f ms), "
                "waiting for the GPU %.2f s, %llu presents holding it up %.2f s (longest %.2f ms); %llu surfaces mapped (%lu shapes); %llu pipelines built (%.2f s, longest %.1f ms); "
                "%llu heap buffers refused, %llu remaps failed, %llu surface pages misaligned\n",
                (unsigned long long)execs, (unsigned long long)draws, (unsigned long long)atomic_load(&failed), (unsigned long long)synced,
                (unsigned long long)remaps, now_seconds() - t0, encode_time, encode_max * 1e3, gpu_wait_time,
                (unsigned long long)atomic_exchange(&present_holds, 0), atomic_exchange(&present_hold_ns, 0) * 1e-9,
                atomic_exchange(&present_hold_max_ns, 0) * 1e-6, (unsigned long long)surfaces_made,
                (unsigned long)surface_shapes.count, (unsigned long long)pipelines_built, pipeline_time, pipeline_max * 1e3,
                (unsigned long long)heap_buffers_refused, (unsigned long long)remaps_failed,
                (unsigned long long)pages_misaligned);
        if (video_scans || video_backs)
            fprintf(stderr, "hengevm: pvg-metal: video: %llu checked for changes (%.2f s, %llu MB of the guest's), %llu uploads (%llu with no flag 1, %llu onto the "
                    "GPU's drawing; %llu rows, %.2f s), %llu waits for the GPU (%.2f s), %llu read back (%llu rows, %.2f s)\n",
                    (unsigned long long)video_scans, video_scan_time, (unsigned long long)(video_bytes_compared >> 20),
                    (unsigned long long)video_uploads, (unsigned long long)video_unsaid, (unsigned long long)video_merges,
                    (unsigned long long)video_rows, video_upload_time, (unsigned long long)video_waits, video_wait_time,
                    (unsigned long long)video_backs, (unsigned long long)video_back_rows, video_back_time);
        encode_time = gpu_wait_time = encode_max = 0;
        pipelines_built = 0, pipeline_time = pipeline_max = 0;
    }
    return YES;
}

/* Every possible half-float value to a display byte, as the screen would show it in the current mode (see hv_half_mode). */
static const uint8_t *half_to_byte(void) {
    static uint8_t *tables[2];
    int mode = hv_half_mode_get();
    if (tables[mode]) return tables[mode];
    uint8_t *t = malloc(65536);
    for (uint32_t i = 0; i < 65536; i++) {
        uint16_t bits = (uint16_t)i;
        __fp16 h;
        memcpy(&h, &bits, 2);
        float v = h;
        t[i] = !(v > 0) ? 0 : v >= 1 ? 255 : (uint8_t)(hv_half_shown(v, mode) * 255.0f + 0.5f);
    }
    tables[mode] = t;
    return t;
}

/* Op 4: a new pointer image in the kernel task's memory, premultiplied BGRA:
   {port, 0, offset, mapped length, stride, width, height, hot x, hot y, sum}. */
- (void)cursorGlyph:(const uint32_t *)a length:(uint32_t)n {
    if (n < 0x2c) return;
    uint64_t off, stride;
    memcpy(&off, a + 2, 8), memcpy(&stride, a + 6, 8);
    uint16_t w, h, hx, hy;
    memcpy(&w, (const uint8_t *)a + 0x20, 2), memcpy(&h, (const uint8_t *)a + 0x22, 2);
    memcpy(&hx, (const uint8_t *)a + 0x24, 2), memcpy(&hy, (const uint8_t *)a + 0x26, 2);
    if (!w || !h || w > 512 || h > 512 || stride < (uint64_t)w * 4 || stride > 8192) return;
    uint8_t *bits = malloc((size_t)stride * h);
    if (hv_pvg_lite_task_read(lite, 0, off, bits, (uint32_t)stride * h)) {
        free(bits);
        [self complain:@"pointer image unreadable"];
        return;
    }
    MTLTextureDescriptor *ct = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    ct.storageMode = MTLStorageModeShared;
    id<MTLTexture> glyph = [device newTextureWithDescriptor:ct];
    [glyph replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:bits bytesPerRow:(NSUInteger)stride];
    os_unfair_lock_lock(&screen_lock);
    uint8_t *old_cursor = cursor;
    cursor = bits, cursor_w = w, cursor_h = h, cursor_stride = (uint32_t)stride, cursor_hot_x = hx, cursor_hot_y = hy;
    cursor_tex = glyph;
    cursor_shown = YES;
    os_unfair_lock_unlock(&screen_lock);
    free(old_cursor);
    if (env.cursor_log) fprintf(stderr, "pvgcursor glyph %ux%u hot %d,%d stride %u first alpha %u\n", w, h, hx, hy, (uint32_t)stride, bits[(size_t)(h / 2) * stride + (size_t)(w / 2) * 4 + 3]);
    [self drawCursor];
}

- (BOOL)hasDisplayLayer {
    os_unfair_lock_lock(&screen_lock);
    BOOL attached = layer != nil;
    os_unfair_lock_unlock(&screen_lock);
    return attached;
}

/* Puts back what the pointer covered, and draws it where the shared page says it is now. */
- (void)drawCursor {
    if ([self hasDisplayLayer]) {
        [self screenChanged];
        return;
    }
    os_unfair_lock_lock(&frame_lock);
    [self drawCursorLocked];
    os_unfair_lock_unlock(&frame_lock);
}

/* The glyph's pixel (0 to n - 1) a frame pixel p shows, for a pointer drawn from origin, each glyph pixel step frame
   pixels wide (its scale times the frame's pixels to one of pointer space's): in the glyph's own space, so a scaled
   picture stretches the glyph as it does the hot spot, and never reads past it (gfx-commands F1). The nearest to the
   pixel's centre: shrunk, the glyph's last row and column are still shown. */
static uint32_t cursor_glyph_at(int p, int origin, double step, uint32_t n) {
    double g = (p - origin + 0.5) / step;
    uint32_t i = g > 0 ? (uint32_t)g : 0;
    return i < n ? i : n - 1;
}

/* drawCursor's pixels: frame_lock held. */
- (void)drawCursorLocked {
    if (!frame || !base) return;
    uint32_t w = frame_w, h = frame_h;
    for (int y = drawn_y; y < drawn_y + drawn_h; y++) memcpy(frame + (size_t)y * w * 4 + (size_t)drawn_x * 4, base + (size_t)y * w * 4 + (size_t)drawn_x * 4, (size_t)drawn_w * 4);
    drawn_w = drawn_h = 0;
    const uint8_t *page = hv_pvg_lite_display_page(lite);
    if (!page || !cursor || pointer_hidden) return;
    uint32_t where = __atomic_load_n((const uint32_t *)(page + 0xe00), __ATOMIC_ACQUIRE);
    if (where == 0xffffffff) return;
    static uint32_t last_draw_where = 0xffffffff;
    BOOL moved = (where != last_draw_where);
    if (moved) cursor_shown = YES;
    last_draw_where = where;
    if (!(cursor_shown || page[0xe04] || moved)) return;
    if (env.cursor_log) {
        static uint32_t last;
        if (where != last) fprintf(stderr, "pvgcursor at %u,%u shown %d flag %d\n", where & 0xffff, where >> 16, cursor_shown, page[0xe04]);
        last = where;
    }
    /* Bigger than the guest draws it. The hot spot stays where it points. Its position and glyph are in
       the display mode's pixels; a scaled picture is in its own. */
    uint32_t pw, ph;
    hv_pvg_lite_pointer_space(w, h, &pw, &ph);
    const double fx = (double)w / pw, fy = (double)h / ph;
    const int scale = cursor_scale();
    int x = (int)(((where & 0xffff) - cursor_hot_x * scale) * fx), y = (int)(((where >> 16) - cursor_hot_y * scale) * fy);
    int cw = (int)(cursor_w * scale * fx + 0.5), ch = (int)(cursor_h * scale * fy + 0.5);
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + cw > (int)w ? (int)w : x + cw, y1 = y + ch > (int)h ? (int)h : y + ch;
    if (x1 <= x0 || y1 <= y0) return;
    for (int py = y0; py < y1; py++) {
        const uint8_t *row = cursor + (size_t)cursor_glyph_at(py, y, scale * fy, cursor_h) * cursor_stride;
        uint8_t *dst = frame + (size_t)py * w * 4 + (size_t)x0 * 4;
        for (int px = x0; px < x1; px++, dst += 4) {
            const uint8_t *src = row + (size_t)cursor_glyph_at(px, x, scale * fx, cursor_w) * 4;
            uint32_t keep = 255u - src[3];
            dst[0] = (uint8_t)(src[0] + (dst[0] * keep + 127) / 255);
            dst[1] = (uint8_t)(src[1] + (dst[1] * keep + 127) / 255);
            dst[2] = (uint8_t)(src[2] + (dst[2] * keep + 127) / 255);
        }
    }
    drawn_x = x0, drawn_y = y0, drawn_w = x1 - x0, drawn_h = y1 - y0;
}

static NSString *const screen_shaders = @"#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 at [[position]]; float2 uv; };\n"
    "vertex V screen_v(uint i [[vertex_id]], constant float4 &rect [[buffer(0)]]) {\n"
    "  float2 c = float2(i & 1, (i >> 1) & 1);\n"
    "  V o; o.at = float4(rect.xy + c * rect.zw, 0, 1); o.uv = float2(c.x, 1 - c.y); return o; }\n"
    "fragment float4 screen_plain(V v [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return float4(t.sample(s, v.uv).rgb, 1); }\n"
    "fragment float4 screen_linear(V v [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant uint &mode [[buffer(0)]]) {\n"
    "  float3 c = saturate(t.sample(s, v.uv).rgb);\n"
    "  if (mode == 1) c = select(1.055 * powr(c, 1.0 / 2.4) - 0.055, c * 12.92, c <= 0.0031308);\n"
    "  return float4(c, 1); }\n"
    "fragment float4 screen_cursor(V v [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
    "  return t.sample(s, v.uv); }\n"
    /* A screen of another size than the layer's pixels, fitted the way a display's scaler does it: Lanczos
       (a = 2) one way at a time, its reach widened when shrinking so that no detail turns to shimmer.
       k: x source/target length along the way it goes, y the kernel's scale (1 growing, below 1 shrinking),
       z its reach in source pixels, w 0 across or 1 down. Rows (or columns) match one to one the other way. */
    "static float lanczos2(float x) {\n"
    "  x = abs(x); if (x < 1e-4) return 1; if (x >= 2) return 0;\n"
    "  float p = M_PI_F * x; return 2 * sin(p) * sin(p * 0.5) / (p * p); }\n"
    "static float4 resample(float2 at, texture2d<float> t, float4 k) {\n"
    "  bool down = k.w > 0.5;\n"
    "  float c = (down ? at.y : at.x) * k.x - 0.5;\n"
    "  int line = int(down ? at.x : at.y), last = int(down ? t.get_height() : t.get_width()) - 1;\n"
    "  int first = int(floor(c - k.z)) + 1, end = min(int(floor(c + k.z)), first + 31);\n"
    "  float4 sum = 0; float weights = 0;\n"
    "  for (int i = first; i <= end; i++) {\n"
    "    float w = lanczos2((float(i) - c) * k.y);\n"
    "    int j = clamp(i, 0, last);\n"
    "    sum += w * t.read(down ? uint2(line, j) : uint2(j, line));\n"
    "    weights += w; }\n"
    "  return sum / max(weights, 1e-4); }\n"
    "fragment float4 screen_resample_h(V v [[stage_in]], texture2d<float> t [[texture(0)]], constant float4 &k [[buffer(0)]]) {\n"
    "  return resample(v.at.xy, t, k); }\n"
    "fragment float4 screen_resample_v(V v [[stage_in]], texture2d<float> t [[texture(0)]], constant float4 &k [[buffer(0)]], constant uint &mode [[buffer(1)]]) {\n"
    "  float3 c = saturate(resample(v.at.xy, t, k).rgb);\n"
    "  if (mode == 1) c = select(1.055 * powr(c, 1.0 / 2.4) - 0.055, c * 12.92, c <= 0.0031308);\n"
    "  return float4(c, 1); }\n";

- (BOOL)prepareScreenForLayer:(CAMetalLayer *)target {
    if (!screen_pipe || !linear_pipe || !cursor_pipe) {
        NSError *err = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:screen_shaders options:nil error:&err];
        if (!lib) { [self complain:[NSString stringWithFormat:@"screen shaders: %@", err.localizedDescription]]; return NO; }
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"screen_v"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        d.fragmentFunction = [lib newFunctionWithName:@"screen_plain"];
        screen_pipe = [device newRenderPipelineStateWithDescriptor:d error:&err];
        d.fragmentFunction = [lib newFunctionWithName:@"screen_linear"];
        linear_pipe = [device newRenderPipelineStateWithDescriptor:d error:&err];
        d.fragmentFunction = [lib newFunctionWithName:@"screen_resample_v"];
        resample_v = [device newRenderPipelineStateWithDescriptor:d error:&err];
        d.fragmentFunction = [lib newFunctionWithName:@"screen_resample_h"];
        resample_h8 = [device newRenderPipelineStateWithDescriptor:d error:&err];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        resample_h16 = [device newRenderPipelineStateWithDescriptor:d error:&err];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        d.fragmentFunction = [lib newFunctionWithName:@"screen_cursor"];
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        cursor_pipe = [device newRenderPipelineStateWithDescriptor:d error:&err];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        screen_sampler = [device newSamplerStateWithDescriptor:sd];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        cursor_sampler = [device newSamplerStateWithDescriptor:sd];
        if (!screen_pipe || !linear_pipe || !cursor_pipe || !screen_sampler || !cursor_sampler || !resample_v || !resample_h8 || !resample_h16) { [self complain:[NSString stringWithFormat:@"screen pipelines: %@", err.localizedDescription]]; return NO; }
    }
    if (!screen_pass) {
        screen_pass = [MTLRenderPassDescriptor new];
        screen_pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        screen_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    if (!resample_pass) {
        resample_pass = [MTLRenderPassDescriptor new];
        resample_pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        resample_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    // Pipelines are shared, but a newly connected monitor has a fresh layer.
    if (prepared_layer != target) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        target.device = device;
        target.pixelFormat = MTLPixelFormatBGRA8Unorm;
        target.framebufferOnly = !(env.gpu_test || env.color_probe); /* the probe copies the drawable out */
        target.allowsNextDrawableTimeout = YES;
        target.maximumDrawableCount = 3;
        [CATransaction commit];
        prepared_layer = target;
        fprintf(stderr, "hengevm: pvg-metal: half-float display surfaces are shown %s (HV_PVG_HALF=%s)\n",
                hv_half_mode_get() == 1 ? "as LINEAR light with an sRGB encode" : "as display-encoded values, passed through", hv_half_mode_get() == 1 ? "linear" : "encoded/default");
        if (cp.on) cp_log_layer(self, target);
    }
    return YES;
}

/* The display, drawn by the GPU. Waiting for a drawable can take until the screen's next
   refresh, so that happens on a thread of its own and never holds up the guest's GPU work:
   this only says what to show next. Whatever is newest when a drawable comes is what is shown. */
- (void)kickPresenter:(BOOL)fresh {
    /* screen_lock held */
    if (fresh) screen_requests++;
    if (presenter_running || !layer || !screen_tex || drawable_retry_pending) return;
    if (drawable_retry_cooldown) {
        double delay = drawable_retry_cooldown - now_seconds();
        if (delay > 0) {
            if (!fresh) return;
            // Coalesce fresh work arriving during cooldown; it may be the guest's
            // last update. Automatic retries alone never reopen an exhausted budget.
            drawable_retry_pending = YES;
            uint64_t generation = layer_generation;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), presenter, ^{
                os_unfair_lock_lock(&self->screen_lock);
                if (self->layer_generation == generation) {
                    self->drawable_retry_pending = NO;
                    self->drawable_retry_cooldown = self->drawable_retry_deadline = 0;
                    [self kickPresenter:NO];
                }
                os_unfair_lock_unlock(&self->screen_lock);
            });
            return;
        }
        drawable_retry_cooldown = drawable_retry_deadline = 0;
    }
    presenter_running = YES;
    dispatch_async(presenter, ^{ [self presentLoop]; });
}

- (void)showOnLayer:(uint64_t)mapping swappedAt:(uint64_t)swap_ns {
    HvObject *surface = hv_pvg_lite_old_driver(lite) ? [self currentSurface:mapping] : (surfaces[@(mapping)] ?: [self newSurface:mapping]);
    if (!surface) { [self complain:[NSString stringWithFormat:@"swap to unknown surface %llu", mapping]]; return; }
    on_screen = mapping;
    BOOL half = surface->fourcc == 'RGhA';
    if (!screen_tex) fprintf(stderr, "hengevm: display scanout format %s (%ux%u)\n",
                             half ? "RGBA16Float" : "BGRA8Unorm", surface->surface_w, surface->surface_h);
    if (!surface->scanout) {
        MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:half ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm
                                                                                     width:surface->surface_w height:surface->surface_h mipmapped:NO];
        t.storageMode = MTLStorageModeShared;
        t.usage = MTLTextureUsageShaderRead;
        id<MTLBuffer> over = surface->object; /* a surface's object is the buffer over its pages */
        if (!over || [self refuse:t what:@"scanout" buffer:over.length offset:surface->first row:surface->row]) return;
        if (linear_texture_fits(device, t, surface->first, surface->row, over.length))
            surface->scanout = [over newTextureWithDescriptor:t offset:surface->first bytesPerRow:surface->row];
        if (!surface->scanout) { [self complain:[NSString stringWithFormat:@"no scanout for surface %llu, row %llu", mapping, surface->row]]; return; }
    }
    os_unfair_lock_lock(&screen_lock);
    screen_tex = surface->scanout, screen_half = half, screen_w = surface->surface_w, screen_h = surface->surface_h;
    screen_dirty = YES;
    if (swap_ns) { /* a new picture from the Mac: the one waiting, if any, will never be shown */
        if (swap_waiting_ns) hv_frame_replaced();
        swap_waiting_ns = swap_ns;
    }
    [self kickPresenter:YES];
    os_unfair_lock_unlock(&screen_lock);
}

/* The pointer moved or changed: the screen again, with no need to go through the GPU queue's worker. */
- (void)screenChanged {
    os_unfair_lock_lock(&screen_lock);
    if (screen_tex) {
        screen_dirty = YES;
        [self kickPresenter:YES];
    }
    os_unfair_lock_unlock(&screen_lock);
}

/* The texture the resampler goes across into: as wide as the drawable, as tall as the screen */
static id<MTLTexture> make_resample_mid(HvReplay *r, uint32_t w, uint32_t h, BOOL half) {
    MTLPixelFormat format = half ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
    if (!r->resample_mid || r->resample_mid.width != w || r->resample_mid.height != h || r->resample_mid.pixelFormat != format) {
        MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
        t.storageMode = MTLStorageModePrivate;
        t.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        r->resample_mid = [r->device newTextureWithDescriptor:t];
    }
    return r->resample_mid;
}

- (void)presentLoop {
    for (;;) @autoreleasepool {
        os_unfair_lock_lock(&screen_lock);
        if (!screen_dirty || !layer || !screen_tex) { /* no screen_tex: its surface was unmapped, nothing to show (5c) */
            if (!layer) atomic_store_explicit(&st_drawable_since, 0, memory_order_relaxed); /* nothing to show it on */
            presenter_running = NO;
            os_unfair_lock_unlock(&screen_lock);
            return;
        }
        screen_dirty = NO;
        CAMetalLayer *target = layer; // retained across nextDrawable and a concurrent disconnect
        uint64_t generation = layer_generation, requests = screen_requests;
        id<MTLTexture> tex = screen_tex, ctex = cursor_tex;
        BOOL half = screen_half, shown = cursor_shown, hidden = pointer_hidden;
        uint32_t W = screen_w, H = screen_h, cw0 = cursor_w, ch0 = cursor_h;
        int hx = cursor_hot_x, hy = cursor_hot_y;
        os_unfair_lock_unlock(&screen_lock);
        int heat = hv_thermal_get_state();
        if (heat >= 2) {
            static uint64_t last_present_ns;
            uint64_t min_interval_ns = heat >= 3 ? 31000000ull : 15500000ull;
            uint64_t now_ns = stage_now();
            if (last_present_ns && now_ns < last_present_ns + min_interval_ns) {
                uint64_t wait_ns = (last_present_ns + min_interval_ns) - now_ns;
                struct timespec ts = {.tv_sec = (time_t)(wait_ns / 1000000000ull), .tv_nsec = (long)(wait_ns % 1000000000ull)};
                nanosleep(&ts, NULL);
            }
            last_present_ns = stage_now();
        }
        if (![self prepareScreenForLayer:target]) {
            os_unfair_lock_lock(&screen_lock);
            BOOL changed = layer_generation != generation;
            if (!changed) {
                presenter_running = NO;
                [self drainPresentThen];
            }
            os_unfair_lock_unlock(&screen_lock);
            if (changed) continue;
            return;
        }
        /* On a Retina panel macOS's other looks (a mode bigger or smaller than the panel, or a scaled
           resolution) are fitted to the layer's own pixels by the resampler, not Core Animation's plain
           bilinear, so text stays as crisp as the panel's own size; at that size it is one to one. The
           1x screens (Fit, Performance, an iPhone) are chosen for speed and stay with Core Animation. */
        uint32_t DW = W, DH = H;
        BOOL fit = NO;
        if (hv_pvg_lite_retina() && !env.gpu_test && !env.color_probe) {
            CGSize bounds = target.bounds.size;
            CGFloat pixels = target.contentsScale;
            double sx = bounds.width * pixels / W, sy = bounds.height * pixels / H, s = sx < sy ? sx : sy;
            uint32_t fw = (uint32_t)(W * s + 0.5), fh = (uint32_t)(H * s + 0.5);
            BOOL same = (fw + 1 >= W && fw <= W + 1) && (fh + 1 >= H && fh <= H + 1);
            /* Zoomed in, the layer is far bigger than any screen: Core Animation enlarges that, as before. */
            if (s > 0 && fw >= 64 && fh >= 64 && fw <= 4096 && fh <= 4096 && !same) DW = fw, DH = fh, fit = YES;
        }
        if (fit && (resample_logged_w != W || resample_logged_h != H)) { /* once a mode: not every frame of a zoom */
            resample_logged_w = W, resample_logged_h = H;
            fprintf(stderr, "hengevm: display: %ux%u fitted to %ux%u pixels\n", W, H, DW, DH);
        }
        CGSize want = CGSizeMake(DW, DH);
        if (!CGSizeEqualToSize(target.drawableSize, want)) {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            target.drawableSize = want;
            [CATransaction commit];
        }
        if (!atomic_load_explicit(&st_drawable_since, memory_order_relaxed))
            atomic_store_explicit(&st_drawable_since, stage_now(), memory_order_relaxed);
        id<CAMetalDrawable> drawable = [target nextDrawable]; /* the wait is here */
        if (drawable) atomic_store_explicit(&st_drawable_since, 0, memory_order_relaxed);
        if (!drawable) {
            os_unfair_lock_lock(&screen_lock);
            screen_dirty = YES;
            BOOL changed = layer_generation != generation;
            if (!changed) {
                presenter_running = NO;
                double now = now_seconds();
                if (!drawable_retry_deadline) drawable_retry_deadline = now + 2;
                BOOL exhausted = now >= drawable_retry_deadline;
                if (exhausted) {
                    drawable_retry_cooldown = now + 1;
                    /* Given up: nothing wants a drawable until new work kicks the presenter after the cool-down, so
                       no wait goes on (a hidden layer would otherwise be held up for as long as the screen is still) */
                    atomic_store_explicit(&st_drawable_since, 0, memory_order_relaxed);
                    // A fresh frame/cursor event received while nextDrawable waited
                    // must not lose its wakeup at the transition into cooldown.
                    if (screen_requests != requests) [self kickPresenter:YES];
                    else [self drainPresentThen];
                } else {
                    // A monitor can connect before its first drawable is available.
                    // Retry even if the guest is idle, but never poll a hidden target forever.
                    drawable_retry_pending = YES;
                    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), presenter, ^{
                        os_unfair_lock_lock(&self->screen_lock);
                        if (self->layer_generation == generation) {
                            self->drawable_retry_pending = NO;
                            [self kickPresenter:NO];
                        }
                        os_unfair_lock_unlock(&self->screen_lock);
                    });
                }
            }
            os_unfair_lock_unlock(&screen_lock);
            if (changed) continue;
            return;
        }
        /* Something newer came during the wait? Then that is the frame to show. The buffer is made first: never
           under screen_lock ([queue commandBuffer] waits while the queue is full; afterGPU). */
        BOOL drawable_fits = drawable.texture.width == DW && drawable.texture.height == DH; /* else an old drawable */
        /* What may wait is done before the pick, too: from presenter_take to the commit the guest's GPU work waits behind
           this frame, so nothing there waits (a texture made after a mode change; settings, which can go to cfprefsd) */
        const BOOL made_half = half; /* what is made here is for this format: the pick goes round again if it changed */
        id<MTLTexture> mid = fit ? make_resample_mid(self, DW, H, made_half) : nil;
        const float scale = cursor_scale();
        const BOOL sharp = cursor_sharp_cached();
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        os_unfair_lock_lock(&screen_lock);
        tex = screen_tex, half = screen_half, ctex = cursor_tex, shown = cursor_shown, hidden = pointer_hidden;
        cw0 = cursor_w, ch0 = cursor_h, hx = cursor_hot_x, hy = cursor_hot_y;
        uint64_t swap_ns = 0; /* the Mac's picture this present carries, if a new one (frame_pace.h) */
        PresenterPick pick = presenter_pick(self, generation, target, W, H, drawable_fits, made_half, nil, &swap_ns);
        const uint64_t held_from = stage_now(); /* a DRAW has taken its place in the GPU's order (presenter_take) */
        os_unfair_lock_unlock(&screen_lock);
        if (pick == PICK_STOP) {
            os_unfair_lock_lock(&screen_lock);
            [self drainPresentThen];
            os_unfair_lock_unlock(&screen_lock);
            return; /* cb never enqueued, the drawable never presented: the layer keeps its last frame */
        }
        if (pick == PICK_AGAIN) {
            os_unfair_lock_lock(&screen_lock);
            [self drainPresentThen];
            os_unfair_lock_unlock(&screen_lock);
            continue; /* cb never enqueued: let go */
        }
        float rect[4] = {-1, -1, 2, 2};
        if (fit && !mid) fit = NO; /* made before the pick, for the same format (nothing is made in the hold); none (no memory for it): the screen is drawn straight into the drawable, stretched by its sampler */
        if (fit) {
            MTLRenderPassDescriptor *across = resample_pass ?: [MTLRenderPassDescriptor renderPassDescriptor];
            across.colorAttachments[0].texture = mid;
            across.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            across.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> a = [cb renderCommandEncoderWithDescriptor:across];
            [a setRenderPipelineState:half ? resample_h16 : resample_h8];
            [a setVertexBytes:rect length:sizeof rect atIndex:0];
            [a setFragmentTexture:tex atIndex:0];
            float kx = (float)W / DW, sx = kx > 1 ? 1 / kx : 1;
            float k[4] = {kx, sx, 2 / sx, 0};
            [a setFragmentBytes:k length:sizeof k atIndex:0];
            [a drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            [a endEncoding];
        }
        MTLRenderPassDescriptor *pass = screen_pass ?: [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
        uint32_t half_mode = half ? (uint32_t)hv_half_mode_get() : 0;
        [e setVertexBytes:rect length:sizeof rect atIndex:0];
        if (fit) { /* then down, into the drawable */
            [e setRenderPipelineState:resample_v];
            [e setFragmentTexture:mid atIndex:0];
            float ky = (float)H / DH, sy = ky > 1 ? 1 / ky : 1;
            float k[4] = {ky, sy, 2 / sy, 1};
            uint32_t encode = half && half_mode == 1;
            [e setFragmentBytes:k length:sizeof k atIndex:0];
            [e setFragmentBytes:&encode length:sizeof encode atIndex:1];
        } else {
            [e setRenderPipelineState:half ? linear_pipe : screen_pipe];
            [e setFragmentTexture:tex atIndex:0];
            [e setFragmentSamplerState:screen_sampler atIndex:0];
            if (half) [e setFragmentBytes:&half_mode length:sizeof half_mode atIndex:0];
        }
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        const uint8_t *page = hv_pvg_lite_display_page(lite);
        uint32_t where = page ? __atomic_load_n((const uint32_t *)(page + 0xe00), __ATOMIC_ACQUIRE) : 0xffffffff;
        static uint32_t last_where = 0xffffffff;
        BOOL cursor_moved = (where != 0xffffffff && where != last_where);
        if (cursor_moved) {
            shown = YES;
            os_unfair_lock_lock(&screen_lock);
            cursor_shown = YES;
            os_unfair_lock_unlock(&screen_lock);
        }
        last_where = where;
        if (ctex && where != 0xffffffff && !hidden && (shown || page[0xe04] || cursor_moved)) {
            /* Placed in the display mode's pixels, which a scaled picture isn't in. */
            uint32_t PW, PH;
            hv_pvg_lite_pointer_space(W, H, &PW, &PH);
            if (PW && PH && cw0 && ch0) {
                float x = (float)(where & 0xffff) - hx * scale, y = (float)(where >> 16) - hy * scale;
                float cw = cw0 * scale, ch = ch0 * scale;
                float r[4] = {x / PW * 2 - 1, 1 - (y + ch) / PH * 2, cw / PW * 2, ch / PH * 2};
                [e setRenderPipelineState:cursor_pipe];
                [e setVertexBytes:r length:sizeof r atIndex:0];
                [e setFragmentTexture:ctex atIndex:0];
                /* Sharp is pixel for pixel; fitted to another size, that would leave it ragged. */
                [e setFragmentSamplerState:fit || !sharp ? screen_sampler : cursor_sampler atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
        }
        [e endEncoding];
        if (cp.on) cp_capture_post(self, drawable.texture, W, H, cb);
#if !TARGET_OS_IPHONE
        if (env.gpu_test) { /* the same pixels for a screenshot: this is the path a phone runs */
            NSUInteger bytes = (NSUInteger)W * H * 4;
            if (!test_readback || test_readback.length < bytes) test_readback = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            id<MTLBuffer> rb = test_readback;
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:drawable.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                      toBuffer:rb destinationOffset:0 destinationBytesPerRow:(NSUInteger)W * 4 destinationBytesPerImage:bytes];
            [b endEncoding];
            [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                (void)done;
                dispatch_async(self->worker, ^{
                    uint8_t *old = NULL, *old_base;
                    os_unfair_lock_lock(&self->frame_lock);
                    if (!self->frame || self->frame_w != W || self->frame_h != H) {
                        old = self->frame;
                        self->frame = malloc((size_t)W * H * 4), self->frame_w = W, self->frame_h = H;
                        if (!self->frame) self->frame_w = self->frame_h = 0;
                    }
                    if (self->frame) memcpy(self->frame, rb.contents, (size_t)W * H * 4);
                    /* The CPU path's frame without the pointer (base, and where the pointer was drawn) is not this one's,
                       nor of its size: the next CPU-drawn frame makes both again (present:) */
                    old_base = self->base, self->base = NULL, self->drawn_w = self->drawn_h = 0;
                    os_unfair_lock_unlock(&self->frame_lock);
                    free(old), free(old_base);
                    self->frames++;
                });
            }];
        }
#endif
        presenter_commit(self, generation, target, cb, drawable, swap_ns, held_from);
    }
}

/* How long a present held the guest's GPU work up behind it, for the stats line and the benchmark (presenter only). */
- (void)noteHold:(uint64_t)ns {
    atomic_fetch_add_explicit(&present_hold_ns, ns, memory_order_relaxed);
    uint64_t longest = atomic_load_explicit(&present_hold_max_ns, memory_order_relaxed);
    while (ns > longest && !atomic_compare_exchange_weak_explicit(&present_hold_max_ns, &longest, ns, memory_order_relaxed,
                                                                  memory_order_relaxed)) {
    }
    atomic_fetch_add_explicit(&present_holds, 1, memory_order_relaxed);
    if (hv_frame_watching()) hv_frame_held(ns);
}

/* A picture taken to be shown that wasn't after all (under screen_lock): it waits again, unless a newer one came
   meanwhile, which replaces it (frame_pace.h). */
- (void)keepWaitingSwap:(uint64_t)swap_ns {
    if (!swap_ns) return;
    if (swap_waiting_ns) hv_frame_replaced();
    else swap_waiting_ns = swap_ns;
}

- (void)drainPresentThen {
    if (!present_then.count) return;
    NSArray *callbacks = [present_then copy];
    [present_then removeAllObjects];
    dispatch_queue_t cq = complete_q;
    dispatch_async(cq, ^{
        for (void (^blk)(void) in callbacks) blk();
    });
}

/* What the display should show: at most a few times a second by CPU, for tests and the boot's first frames. */
- (void)present:(uint64_t)mapping { [self present:mapping swappedAt:0]; }

/* swap_ns: the Mac's swap that asked for it (0: shown again for another reason) */
- (void)present:(uint64_t)mapping swappedAt:(uint64_t)swap_ns {
    double now = now_seconds();
    /* An unmapped id is not brought back to the screen (a late present below, or set_layer's): unmap let it go (5c) */
    HvObject *surface = hv_pvg_lite_old_driver(lite) ? [self currentSurface:mapping] : (surfaces[@(mapping)] ?: [self newSurface:mapping]);
    if (!surface) { [self complain:[NSString stringWithFormat:@"swap to unknown surface %llu", mapping]]; return; }
    on_screen = mapping;
    if ([self hasDisplayLayer]) {
        late = NO;
        [self showOnLayer:mapping swappedAt:swap_ns];
        return;
    }
    if (now - last_capture < 0.066) { /* 15 a second is plenty until this is GPU work */
        late_mapping = mapping, late = YES;
        if (!late_queued) {
            late_queued = YES;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 70 * NSEC_PER_MSEC), worker, ^{
                self->late_queued = NO;
                if (self->late) [self present:self->late_mapping];
            });
        }
        return;
    }
    late = NO;
    last_capture = now;
    if (!cb_wait(self, last_cb, HV_PVG_GPU_WAIT, NULL)) { /* the CPU copies the pixels here, so the GPU must be done with them */
        atomic_fetch_add(&gpu_waits_given_up, 1);
        [self complain:@"the GPU's last work still running after its longest wait: frames skipped until it is done"];
        return; /* no frame: the one shown stays */
    }
    NSUInteger bpp = surface->fourcc == 'RGhA' ? 8 : 4, row = surface->row;
    uint32_t w = surface->surface_w, h = surface->surface_h;
    /* The pixels read are the surface's: its rows, within its pages */
    if (!w || !h || !surface->mapped || row < (uint64_t)w * bpp || surface->first > surface->length || h > (surface->length - surface->first) / row) {
        [self complain:[NSString stringWithFormat:@"swap to surface %llu: %ux%u, row %lu, from %u, past its %llu bytes", mapping, w, h,
                        (unsigned long)row, surface->first, surface->length]];
        return;
    }
    if (!frame || !base || frame_w != w || frame_h != h) {
        /* Readers copy under frame_lock (hv_pvg_metal_frame), so the old one is freed as soon as it is swapped out */
        uint8_t *nbase = malloc((size_t)w * h * 4), *nframe = calloc((size_t)w * h, 4);
        if (!nbase || !nframe) free(nbase), free(nframe), nbase = nframe = NULL;
        os_unfair_lock_lock(&frame_lock);
        uint8_t *old = frame;
        frame = nframe, frame_w = nframe ? w : 0, frame_h = nframe ? h : 0;
        os_unfair_lock_unlock(&frame_lock);
        free(old), free(base);
        base = nbase;
        drawn_w = drawn_h = 0;
        if (!frame) return;
    }
    uint8_t *out = base;
    const uint8_t *src = surface->mapped + surface->first;
    const uint8_t *lut = half_to_byte();
    if (bpp == 8) {
        for (uint32_t y = 0; y < h; y++) {
            const uint16_t *in = (const uint16_t *)(src + y * row);
            uint8_t *o = out + (size_t)y * w * 4;
            for (uint32_t x = 0; x < w; x++, in += 4, o += 4) {
                o[0] = lut[in[2]], o[1] = lut[in[1]], o[2] = lut[in[0]], o[3] = 255;
            }
        }
    } else {
        for (uint32_t y = 0; y < h; y++) memcpy(out + (size_t)y * w * 4, src + y * row, (size_t)w * 4);
    }
    os_unfair_lock_lock(&frame_lock);
    memcpy(frame, base, (size_t)w * h * 4);
    drawn_w = drawn_h = 0;
    os_unfair_lock_unlock(&frame_lock);
    [self drawCursor];
    frames++;
}

#if !TARGET_OS_IPHONE
/* Debugging: HV_PVG_SHOT=prefix keeps what is on the display, every HV_PVG_SHOT_SECS seconds it has changed. */
- (void)saveShot {
    if (!frame || frames == frames_shot) return;
    frames_shot = frames;
    char path[1100];
    snprintf(path, sizeof path, "%s-%04u.png", shot, shots++);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(frame, frame_w, frame_h, 8, (size_t)frame_w * 4, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    if (dest) {
        CGImageDestinationAddImage(dest, img, NULL);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
    }
    CFRelease(url), CGImageRelease(img), CGContextRelease(ctx), CGColorSpaceRelease(cs);
}
#endif

@end

static void read_env(HvReplay *r) {
    r->env.trace_file = getenv("HV_PVG_TRACE_FILE"), r->env.host_dump = getenv("HV_PVG_HOST_DUMP");
    r->env.surface_dump = getenv("HV_PVG_SURFACE_DUMP");
    r->env.fail_libs = getenv("HV_PVG_FAIL_LIBS") != NULL, r->env.shader_dump = getenv("HV_SHADER_DUMP") && atoi(getenv("HV_SHADER_DUMP"));
    r->env.heap_log = getenv("HV_PVG_HEAP_LOG") != NULL, r->env.tally = getenv("HV_PVG_TALLY") != NULL;
    r->env.cursor_log = getenv("HV_PVG_CURSOR_LOG") != NULL, r->env.gpu_test = getenv("HV_PVG_GPU_TEST") != NULL;
    r->env.color_probe = getenv("HV_COLOR_PROBE") != NULL;
}

HvPvgMetal *hv_pvg_metal_create(HvPvgLite *lite) {
    /* nil with HV_NO_GPU or HV_PVG_NO_METAL set (gpu_switch.h); pvg_lite.c's ensure_metal doesn't call this then */
    @autoreleasepool {
        id<MTLDevice> dev = hv_gpu_device();
        if (!dev) return NULL;
        if (![dev respondsToSelector:@selector(newRenderPipelineDescriptorWithSerializedData:deserializationContext:)]) {
            fprintf(stderr, "hengevm: pvg-metal: this Metal cannot read serialized pipelines; no drawing\n");
            return NULL;
        }
        Protocol *proto = objc_getProtocol("MTLDeserializationContext");
        if (proto) class_addProtocol([HvContext class], proto);
        HvReplay *r = [HvReplay new];
        r->lite = lite;
        r->device = dev;
        r->queue = [dev newCommandQueueWithMaxCommandBufferCount:64];
        r->worker = dispatch_queue_create("hengevm.pvg", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
        r->presenter = dispatch_queue_create("hengevm.present", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
        r->screen_lock = OS_UNFAIR_LOCK_INIT;
        r->trail_lock = OS_UNFAIR_LOCK_INIT;
        r->gpu_strike_lock = OS_UNFAIR_LOCK_INIT;
        r->cursor_shown = YES;
        r->answers = [NSMutableDictionary new];
        r->surfaces = [NSMutableDictionary new];
        r->surface_shapes = [NSMutableSet new];
        r->planar =[NSMapTable strongToWeakObjectsMapTable];
        r->planar_kept = [NSMutableArray new];
        r->complaints = [NSMutableSet new];
        r->writeback = [NSMutableArray new];
        /* What this GPU takes: every description, command and size the guest gives is checked against these */
        r->caps.bc = [dev respondsToSelector:@selector(supportsBCTextureCompression)] ? dev.supportsBCTextureCompression : 0;
#if !TARGET_OS_IPHONE
        if (![dev respondsToSelector:@selector(supportsBCTextureCompression)]) r->caps.bc = 1; /* every Mac GPU before it */
#endif
        r->caps.float32_filter = dev.supports32BitFloatFiltering;
        r->caps.samples = 1u << 1;
        for (uint32_t n = 2; n <= 8; n++)
            if ([dev supportsTextureSampleCount:n]) r->caps.samples |= 1u << n;
        r->caps.color_bytes = [dev supportsFamily:MTLGPUFamilyApple7] ? 128 : [dev supportsFamily:MTLGPUFamilyApple4] ? 64
                            : [dev supportsFamily:MTLGPUFamilyApple2] ? 32 : 0;
        r->caps.implicit_bytes = [dev supportsFamily:MTLGPUFamilyApple9] ? 256 << 10 : [dev supportsFamily:MTLGPUFamilyApple4] ? 128 << 10 : 0;
        BOOL many = [dev supportsFamily:MTLGPUFamilyApple6] || [dev supportsFamily:MTLGPUFamilyMac2];
        r->limits = hv_pvg_cmd_limits(many ? 128 : 96, dev.maxThreadgroupMemoryLength);
        r->max_buffer = dev.maxBufferLength;
        r->heap_limit = dev.recommendedMaxWorkingSetSize ? dev.recommendedMaxWorkingSetSize : dev.maxBufferLength;
        r->heap_total = calloc(1, sizeof *r->heap_total);
        if (!r->heap_total) return NULL;
        r->mapper_q = dispatch_queue_create("hengevm.pvg-mapper", DISPATCH_QUEUE_SERIAL);
        r->complete_q = dispatch_queue_create("hengevm.pvg-complete", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
        r->shot = getenv("HV_PVG_SHOT");
        read_env(r);
        cp_start(r);
#if !TARGET_OS_IPHONE
        if (getenv("HV_PVG_GPU_TEST")) r->layer = [CAMetalLayer layer];
#endif
#if !TARGET_OS_IPHONE
        if (r->shot) {
            double every = getenv("HV_PVG_SHOT_SECS") ? atof(getenv("HV_PVG_SHOT_SECS")) : 5;
            dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, r->worker);
            dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(every * NSEC_PER_SEC)), (uint64_t)(every * NSEC_PER_SEC), NSEC_PER_SEC / 10);
            HvReplay *__weak weak = r;
            dispatch_source_set_event_handler(timer, ^{ [weak saveShot]; });
            dispatch_resume(timer);
            r->shot_timer = timer;
        }
#endif
        fprintf(stderr, "hengevm: pvg-metal: drawing on %s\n", dev.name.UTF8String);
        return (HvPvgMetal *)CFBridgingRetain(r);
    }
}

/* What the guest's delete commands drop, read as Apple's host reads them (ParavirtualizedGraphics.framework's
   -[PGFIFO CmdDeleteTask:...], CmdDeleteResource: and CmdDeleteObject:): 0x20 {task} a task and everything made for
   it, 0x25 {task, ref} a resource, 0x28 {task, tag, size, ref} one object (the tag says its kind: 1005 a function).
   Each names its task by its number as it is, as every command does but CmdDefineTask2 (0x38), whose first word is
   that number shifted left one (pvg_lite.c). CmdDeleteObject reads the ref at the same place for every tag. 0x20 was
   read shifted too until 9 Nov 2026, so a closed Mac app's shaders, pipelines and textures stayed here under the number
   the next app to open was given, and that app could be handed them for each object whose 12-byte entry matched one of
   its own (the same type, size and place in its memory). Whether that ever happened is not known: no log shows it, and
   it is not shown to be what stopped the iPad in the GPU driver (pipeline_stage_problem). Deletes may come on a channel
   (hv_pvg_metal_submit) or the root ring (hv_pvg_metal_forget), and the root ring's were dropped unread until 9 Nov
   2026, a second way objects could outlive their task. A define (0x38) forgets its number too: Apple's
   -[_PGDevice createTaskID:taskRoot:length:restoring:] refuses a number in use ("Host task already exists"), so what is
   kept here under a number being defined was missed when it was deleted, and its log line says so. NO for any other
   command; a delete or define too short to name its task is taken and does nothing. */
static BOOL forget_deleted(HvReplay *r, uint32_t op, const uint32_t *args, uint32_t n) {
    if (op == 0x20 || op == 0x38) {
        if (n < (op == 0x38 ? 16u : 4u)) return YES;
        uint32_t task = op == 0x38 ? args[0] >> 1 : args[0];
        NSUInteger objects = task < HV_TASKS && r->tasks[task] ? r->tasks[task]->count : 0, had = r->answers[@(task)].count;
        if (task < HV_TASKS) r->tasks[task] = nil;
        [r->answers removeObjectForKey:@(task)];
        r->answers_count -= (uint32_t)had;
        uint32_t *count = &r->tasks_forgot[op == 0x38];
        if ((objects || had) && power_of_ten(++*count)) /* a guest's loop of run and delete would be a line a delete */
            fprintf(stderr, "hengevm: pvg: task %u %s: %lu objects, %lu answers forgotten; %u such so far\n", task,
                    op == 0x38 ? "defined again before it was deleted" : "deleted", (unsigned long)objects, (unsigned long)had, *count);
        return YES;
    }
    if (op == 0x25 || op == 0x28) {
        if (n < (op == 0x25 ? 8u : 16u)) return YES;
        HvTaskObjects *objs = task_objects(r, args[0], NO);
        if (objs) objs_set(objs, op == 0x25 ? args[1] : args[3], nil);
        return YES;
    }
    return NO;
}

/* A command's arguments, after its header and barriers, and how many bytes of them there are. */
static const uint32_t *command_args(const uint32_t *cmd, uint32_t len, uint32_t *n) {
    uint32_t skip = (cmd[0] >> 16) * 8; /* the barriers come first */
    *n = len > 12 + skip ? len - 12 - skip : 0;
    return (const uint32_t *)((const uint8_t *)cmd + 12 + skip);
}

void hv_pvg_metal_forget(HvPvgMetal *m, uint32_t *cmd, uint32_t len) {
    HvReplay *r = (__bridge HvReplay *)m;
    if (len < 12) { free(cmd); return; }
    dispatch_async(r->worker, ^{
        @autoreleasepool {
            uint32_t n;
            const uint32_t *args = command_args(cmd, len, &n);
            hv_pvg_lite_task_command(r->lite, cmd[0] & 0xffff, args, n);
            forget_deleted(r, cmd[0] & 0xffff, args, n);
            free(cmd);
        }
    });
}

void hv_pvg_metal_submit(HvPvgMetal *m, HvPvgLite *lite, uint32_t ch, uint32_t *cmd, uint32_t len) {
    HvReplay *r = (__bridge HvReplay *)m;
    uint64_t seq = atomic_load_explicit(&r->st_submitted, memory_order_relaxed); /* the device lock's: one submitter */
    atomic_store_explicit(&r->st_queued[seq % HV_PVG_STAGE_RING], stage_now(), memory_order_relaxed);
    atomic_store_explicit(&r->st_submitted, seq + 1, memory_order_release);
    dispatch_async(r->worker, ^{
        atomic_store_explicit(&r->st_worker_since, stage_now(), memory_order_relaxed);
        atomic_store_explicit(&r->st_started, seq + 1, memory_order_release); /* the worker runs them in order */
        @autoreleasepool {
            uint32_t op = cmd[0] & 0xffff, n;
            const uint32_t *args = command_args(cmd, len, &n);
            BOOL gpu_completes = NO;
            hv_pvg_lite_task_command(lite, op, args, n); /* a define or object list: the reads from here on use it */
            @try {
                if ((op == 0x43 || op == 0x45) && n >= 12) {
                    gpu_completes = [r exec:args length:n channel:ch stamp:cmd[2]];
                } else if (op == 0x37 && n >= 12) {
                    /* macOS 13's driver: the same work with 24-byte resource entries ({ref, flags} and four more words)
                       before the 16-byte buffers. Put it in 0x43's shape: {task, resources, buffers, {ref, flags}..., buffers...}. */
                    uint32_t nres = args[1], nbuf = args[2];
                    if (12 + nres * 24ull + nbuf * 16ull <= n && nres < 65536 && nbuf < 65536) {
                        uint32_t len43 = 12 + nres * 8 + nbuf * 16, *a43 = malloc(len43);
                        if (a43) {
                            a43[0] = args[0], a43[1] = nres, a43[2] = nbuf;
                            for (uint32_t i = 0; i < nres; i++) a43[3 + i * 2] = args[3 + i * 6], a43[4 + i * 2] = args[4 + i * 6];
                            memcpy(a43 + 3 + nres * 2, args + 3 + nres * 6, nbuf * 16ull);
                            gpu_completes = [r exec:a43 length:len43 channel:ch stamp:cmd[2]];
                            free(a43);
                        }
                    } else [r complain:[NSString stringWithFormat:@"command 0x37 of %u bytes: %u resources, %u buffers do not fit", n, nres, nbuf]];
                } else if (forget_deleted(r, op, args, n)) {
                } else if (op == 0x35 || op == 0x3e) {
                    [r synchronize:args length:n];
                } else if (op == 0x34) {
                    [r invalidate:args length:n];
                } else if (op == 0x3f) {
                    /* "May be thrown away": Apple's host only marks it purgeable, and the bytes stay
                       until the system needs the memory. Here they just stay. */
                } else {
                    /* 0 for a display command Apple's host doesn't take either (too short, or a port past the
                       one display): nothing to present or draw for it */
                    int taken = hv_pvg_lite_command(lite, cmd, len);
                    if (op == 8 && taken) {
                        if (r->tracing) fprintf(stderr, "trace: swap to %llu\n", (uint64_t)args[3] << 32 | args[2]);
                        cp_note_swap(r, (uint64_t)args[3] << 32 | args[2], args, n);
                        uint64_t swapped = hv_frame_watching() ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
                        if (swapped) hv_pace_note(&hv_pace_mac, swapped); /* a new picture from macOS */
                        [r present:(uint64_t)args[3] << 32 | args[2] swappedAt:swapped];
                    }
                    if (op == 4 && taken) [r cursorGlyph:args length:n];
                    if (op == 5 && taken) {
                        os_unfair_lock_lock(&r->screen_lock);
                        r->cursor_shown = args[1] & 1;
                        os_unfair_lock_unlock(&r->screen_lock);
                        if (r->env.cursor_log) fprintf(stderr, "pvgcursor show %u\n", args[1]);
                        [r drawCursor];
                    }
                }
            } @catch (NSException *x) {
                [r complain:[NSString stringWithFormat:@"exception: %@", x.reason]];
            }
            if (gpu_completes) {
            } else if (op == 8) {
                [r finishSwap:ch stamp:cmd[2]];
            } else {
                [r finishChannel:ch stamp:cmd[2]];
            }
            free(cmd);
        }
        atomic_store_explicit(&r->st_worker_since, 0, memory_order_relaxed);
    });
}

/* The presenter's frame takes its place in the GPU's order at the moment it takes the screen's texture (screen_lock
   held, and the presenter's thread commits it soon after): enqueued now, so whatever is committed after this (the
   guest's next drawing, a trail) runs after it on the GPU, and counted now as GPU work like the guest's (inflight,
   presents), so a stamp or an unmap that waits for the GPU's work so far (afterGPU) waits for it. A swap away from
   the texture gives its stamp back only after this frame has read it (finishSwap); before, the stamp could be given
   back while the presenter was still encoding its read, and the guest draw its next frame into it first. */
/* After nextDrawable, under screen_lock: draw now (the frame taken, with the screen_tex just read), go round again (a
   target or mode change during the wait, or the screen's format: half, what was made for before the pick), or stop. Stop is for a screen let go of during the wait (its surface unmapped,
   5c): nothing to draw from, so nothing is taken or presented and the presenter ends; the next swap starts it again.
   *swap_ns: the Mac's picture waiting for the screen, taken only by a draw; otherwise it goes on waiting (keepWaitingSwap's
   case, under the same lock hold here: nothing newer can come between). */
static PresenterPick presenter_pick(HvReplay *r, uint64_t generation, CAMetalLayer *target, uint32_t W, uint32_t H, BOOL fits,
                                    BOOL half, id<MTLCommandBuffer> cb, uint64_t *swap_ns) {
    if (!r->screen_tex) {
        r->presenter_running = NO;
        return PICK_STOP;
    }
    BOOL same_target = r->layer_generation == generation && r->layer == target;
    BOOL same_size = r->screen_w == W && r->screen_h == H && fits && r->screen_half == half;
    if (!same_target || !same_size) {
        r->screen_dirty = YES; /* a target, mode or format change during nextDrawable */
        return PICK_AGAIN;
    }
    r->screen_dirty = NO;
    r->drawable_retry_deadline = 0;
    *swap_ns = r->swap_waiting_ns, r->swap_waiting_ns = 0;
    if (cb) {
        r->presenter_drawing = NO;
        presenter_take(r, cb); /* with screen_tex: a swap away from it from now on waits for this frame */
    } else {
        r->presenter_drawing = YES;
    }
    return PICK_DRAW;
}

/* The end of a frame taken (presenter_pick's DRAW): presented if the target is still the one it was drawn for, and
   committed either way. Enqueued at the take, the frame holds up all the GPU work committed after it (the guest's next
   drawing, a swap's stamp) until it is committed: a frame left uncommitted would stall the guest for good. The check
   and the commit are serialized with set_layer: a switch that has returned is never followed by a present to its old
   target. Returns whether it was presented. */
static BOOL presenter_commit(HvReplay *r, uint64_t generation, CAMetalLayer *target, id<MTLCommandBuffer> cb,
                             id<CAMetalDrawable> drawable, uint64_t swap_ns, uint64_t held_from) {
    os_unfair_lock_lock(&r->screen_lock);
    uint64_t take_from = held_from;
    if (r->presenter_drawing) {
        r->presenter_drawing = NO;
        take_from = stage_now();
        presenter_take(r, cb);
        if (r->present_then.count) {
            NSArray *draining = [r->present_then copy];
            [r->present_then removeAllObjects];
            dispatch_queue_t cq = r->complete_q;
            [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                (void)done;
                dispatch_async(cq, ^{
                    for (void (^blk)(void) in draining) blk();
                });
            }];
        }
    }
    BOOL same = r->layer_generation == generation && r->layer == target;
    if (same) {
        /* when it was on the glass, for the benchmark (frame_pace.h): 0 if the screen dropped it */
        if (hv_frame_watching())
            [drawable addPresentedHandler:^(id<MTLDrawable> d) {
                CFTimeInterval at = d.presentedTime; /* host time, as CLOCK_UPTIME_RAW */
                hv_frame_presented(at > 0 ? (uint64_t)(at * 1e9) : 0, swap_ns);
            }];
        [cb presentDrawable:drawable];
        r->gpu_frames++;
    } else {
        r->screen_dirty = YES; /* drawn again, for the new target */
        [r keepWaitingSwap:swap_ns];
    }
    const uint64_t hold_ns = stage_now() - take_from;
    os_unfair_lock_unlock(&r->screen_lock);
    [cb commit]; /* either way - outside lock to prevent worker and UI stall */
    [r noteHold:hold_ns];
    return same;
}

static void presenter_take(HvReplay *r, id<MTLCommandBuffer> cb) {
    [cb enqueue];
    gpu_begun(r);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        atomic_store_explicit(&r->st_gpu_since, stage_now(), memory_order_relaxed);
        atomic_fetch_sub(&r->inflight, 1);
    }];
    atomic_fetch_add(&r->presents, 1); /* afterGPU no longer joins an earlier trail */
}

/* GPU work committed: with none in flight before it, the GPU's stretch without a finished buffer starts now. */
static void gpu_begun(HvReplay *r) {
    /* The time first, then the count: a reader that sees work in flight never sees the time of an earlier stretch.
       This leans on every completion storing st_gpu_since before it takes its buffer off inflight: if the last one
       finishes between the load and the add, the time kept is that completion's, not one from before it. */
    if (!atomic_load(&r->inflight)) atomic_store_explicit(&r->st_gpu_since, stage_now(), memory_order_relaxed);
    atomic_fetch_add(&r->inflight, 1);
}

void hv_pvg_metal_stages(HvPvgMetal *m, uint32_t ms[HV_PVG_STAGES]) {
    HvReplay *r = (__bridge HvReplay *)m;
    uint64_t now = stage_now();
    uint64_t sub = atomic_load_explicit(&r->st_submitted, memory_order_acquire), st = atomic_load_explicit(&r->st_started, memory_order_acquire);
    uint64_t since[4] = {0, 0, 0, 0};
    /* The oldest not begun is number st; with a ring or more waiting it was written over, and the oldest still kept is
       the one after sub's place (sub's own is the next submit's to write) */
    if (sub > st) since[0] = atomic_load_explicit(&r->st_queued[(sub - st >= HV_PVG_STAGE_RING ? sub - HV_PVG_STAGE_RING + 1 : st) % HV_PVG_STAGE_RING], memory_order_relaxed);
    since[1] = atomic_load_explicit(&r->st_worker_since, memory_order_relaxed);
    if (atomic_load(&r->inflight) > 0) since[2] = atomic_load_explicit(&r->st_gpu_since, memory_order_relaxed);
    since[3] = atomic_load_explicit(&r->st_drawable_since, memory_order_relaxed);
    const int at[4] = {HV_PVG_STAGE_QUEUED, HV_PVG_STAGE_WORKER, HV_PVG_STAGE_GPU, HV_PVG_STAGE_DRAWABLE};
    for (int i = 0; i < 4; i++) ms[at[i]] = since[i] && now > since[i] ? (uint32_t)MIN((now - since[i]) / 1000000, UINT32_MAX) : 0;
}

void hv_pvg_metal_set_layer(HvPvgMetal *m, void *layer) {
    if (!m) return;
    HvReplay *r = (__bridge HvReplay *)m;
    CAMetalLayer *l = (__bridge CAMetalLayer *)layer;
    // Invalidate immediately, even when the guest GPU worker has a backlog.
    os_unfair_lock_lock(&r->screen_lock);
    r->layer_generation++; /* reattaching the same layer also wakes a stopped presenter */
    r->layer = l;
    r->drawable_retry_pending = NO;
    r->drawable_retry_deadline = r->drawable_retry_cooldown = 0;
    atomic_store_explicit(&r->st_drawable_since, 0, memory_order_relaxed); /* a new layer's wait is its own */
    r->screen_dirty = YES;
    [r kickPresenter:YES];
    os_unfair_lock_unlock(&r->screen_lock);
    dispatch_async(r->worker, ^{
        if (r->on_screen) [r present:r->on_screen];
    });
}

int hv_pvg_metal_screen_size(HvPvgMetal *m, uint32_t *w, uint32_t *h) {
    if (!m) return -1;
    HvReplay *r = (__bridge HvReplay *)m;
    os_unfair_lock_lock(&r->screen_lock);
    uint32_t sw = r->screen_w, sh = r->screen_h;
    os_unfair_lock_unlock(&r->screen_lock);
    if (!sw || !sh) return -1;
    *w = sw, *h = sh;
    return 0;
}

int hv_pvg_metal_gpu_faulted(HvPvgMetal *m) {
    return m ? atomic_load(&((__bridge HvReplay *)m)->gpu_faulted) || guest_work() == GUEST_WORK_REFUSED : 0;
}

void hv_pvg_metal_gpu_rearm(HvPvgMetal *m) {
    if (m) [(__bridge HvReplay *)m gpuRearm];
}

int hv_pvg_metal_gpu_state(HvPvgMetal *m) {
    return m ? [(__bridge HvReplay *)m gpuState] : HV_PVG_GPU_RUNS;
}

int hv_pvg_metal_gpu_try_again(HvPvgMetal *m) {
    return m ? [(__bridge HvReplay *)m gpuTryAgain] : -1;
}

/* The longest an auto-save may hold the worker: past it the guest's graphics go on (and that save can't count on its
   claim of the GPU's pages: hv_pvg_metal_release says so) */
#ifndef HV_PVG_HOLD_MAX
#define HV_PVG_HOLD_MAX (2 * NSEC_PER_SEC)
#endif

int hv_pvg_metal_hold(HvPvgMetal *m, uint64_t wait_ns) {
    if (!m) return 0;
    HvReplay *r = (__bridge HvReplay *)m;
    int none = 0;
    if (!atomic_compare_exchange_strong(&r->held, &none, 1)) return -1; /* held already, or one that ran out not yet released */
    dispatch_semaphore_t parked = dispatch_semaphore_create(0), release = dispatch_semaphore_create(0);
    r->hold_release = release;
    dispatch_async(r->worker, ^{
        dispatch_semaphore_signal(parked);
        if (dispatch_semaphore_wait(release, dispatch_time(DISPATCH_TIME_NOW, (int64_t)HV_PVG_HOLD_MAX))) {
            int held = 1;
            if (atomic_compare_exchange_strong(&r->held, &held, 2))
                fprintf(stderr, "hengevm: pvg-metal: an auto-save held the Mac's graphics too long: they go on\n");
        }
    });
    dispatch_time_t by = dispatch_time(DISPATCH_TIME_NOW, (int64_t)MIN(wait_ns, (uint64_t)INT64_MAX));
    if (dispatch_semaphore_wait(parked, by)) {
        hv_pvg_metal_release(m); /* not parked in time: what runs ahead of it may write the guest's pages; let it go at once */
        return -1;
    }
    /* And what the completions left on complete_q (stamps, video copy-backs) done: parked, the worker adds none */
    while (atomic_load(&r->guest_side) != 0) {
        if (dispatch_time(DISPATCH_TIME_NOW, 0) >= by) { hv_pvg_metal_release(m); return -1; }
        usleep(100);
    }
    return 0;
}

int hv_pvg_metal_release(HvPvgMetal *m) {
    if (!m) return 0;
    HvReplay *r = (__bridge HvReplay *)m;
    dispatch_semaphore_t release = r->hold_release; /* set before the hold returned, by the thread that releases */
    int was = atomic_exchange(&r->held, 0);
    if (!was) return -1;
    r->hold_release = nil;
    dispatch_semaphore_signal(release);
    return was == 1 ? 0 : -1;
}

int hv_pvg_metal_gpu_code_refused(HvPvgMetal *m) {
    return m && atomic_load(&((__bridge HvReplay *)m)->libs_refused) ? 1 : 0;
}

uint64_t hv_pvg_metal_gpu_frames(HvPvgMetal *m) {
    return m ? ((__bridge HvReplay *)m)->gpu_frames : 0;
}

/* As the screen would draw it (the host's own hiding aside): a pointer image from WindowServer, a place for it, and
 * shown by its message or by the flag in the display's page (which is how the login window shows it). */
int hv_pvg_metal_pointer_shown(HvPvgMetal *m) {
    if (!m) return 0;
    HvReplay *r = (__bridge HvReplay *)m;
    if (atomic_load(&r->cursor_ever_shown)) return 1;
    const uint8_t *page = hv_pvg_lite_display_page(r->lite);
    os_unfair_lock_lock(&r->screen_lock);
    BOOL glyph = r->cursor_tex != nil, shown = r->cursor_shown;
    os_unfair_lock_unlock(&r->screen_lock);
    uint32_t where = page ? __atomic_load_n((const uint32_t *)(page + 0xe00), __ATOMIC_ACQUIRE) : 0xffffffff;
    /* The glyph is enough: WindowServer gives one only once the login window (or the desktop) is up. Where it
       is and whether it is drawn are not: the host keeps it in a corner, just off the screen, where macOS
       gives no position for it. */
    if (!glyph) return 0;
    if (!atomic_exchange(&r->cursor_ever_shown, 1))
        fprintf(stderr, "pvg: macOS has its pointer (the login window or the desktop is up): at %d,%d, shown %d, flag %d\n",
                where == 0xffffffff ? -1 : (int)(where & 0xffff), where == 0xffffffff ? -1 : (int)(where >> 16), shown,
                page ? page[0xe04] : -1);
    return 1;
}

void hv_pvg_metal_cursor_moved(HvPvgMetal *m) {
    if (!m) return;
    HvReplay *r = (__bridge HvReplay *)m;
    os_unfair_lock_lock(&r->screen_lock);
    r->cursor_shown = YES;
    os_unfair_lock_unlock(&r->screen_lock);
    if ([r hasDisplayLayer]) { /* no need to queue behind the guest's GPU work */
        [r screenChanged];
        return;
    }
    static _Atomic int queued;
    if (atomic_exchange(&queued, 1)) return; /* one is already on its way, and it will read the newest position */
    dispatch_async(r->worker, ^{
        atomic_store(&queued, 0);
        [r drawCursor];
    });
}

void hv_pvg_metal_hide_pointer(HvPvgMetal *m, int hide) {
    if (!m) return;
    HvReplay *r = (__bridge HvReplay *)m;
    os_unfair_lock_lock(&r->screen_lock);
    BOOL changed = r->pointer_hidden != (hide != 0);
    r->pointer_hidden = hide != 0;
    os_unfair_lock_unlock(&r->screen_lock);
    if (changed) hv_pvg_metal_cursor_moved(m);
}

/* The mapper's entries up to index are done once the GPU has done the work handed to it before them (it may still read
   the pages they unmap): told to pvg_lite from the mapper's own queue, never from the worker or the GPU's threads,
   and never waited for by a vCPU. gen: the mapper's generation then; a reset since makes it stale. */
void hv_pvg_metal_mapper_fence(HvPvgMetal *m, uint64_t index, uint64_t gen) {
    if (!m) return;
    HvReplay *r = (__bridge HvReplay *)m;
    HvPvgLite *lite = r->lite;
    dispatch_queue_t q = r->mapper_q;
    dispatch_async(r->worker, ^{ /* after the work already queued for the worker */
        [r afterGPU:^{ dispatch_async(q, ^{ hv_pvg_lite_mapper_done(lite, index, gen); }); }];
    });
}

/* A surface's mapping gone, on the worker: by the guest's unmap, or (stale) by a map of its id when the surface is
   still here, its unmap passed over by the mapper (a ring restart, or a jump of its index). */
static void drop_surface(HvReplay *r, uint32_t mapping, BOOL stale) {
    HvObject *s = r->surfaces[@(mapping)];
    if (s && s->video) planar_keep(r, s->video); /* soon mapped again by another process, maybe: what was drawn is still there */
    if (s) s->gone = YES; /* textures over it the guest still names are made anew (object:task:) */
    /* and let go of now: kept until their refs were named again, they held the surface's pages and GPU mapping */
    for (HvObject *o in s ? s->over.allObjects : @[]) {
        HvTaskObjects *objs = task_objects(r, o->cache_task, NO);
        if (objs_get(objs, o->cache_ref) == o) objs_set(objs, o->cache_ref, nil);
    }
    if (s) s->over = nil;
    /* Its copy-backs still to come: an unmap's fence waits for them before the guest has its pages back. A stale one
       had no fence, so they go only where another mapping of the same video still has the guest's pages for it (the
       same pages: video_key), and not through this one alone. */
    if (stale && s && s->video) {
        BOOL elsewhere = NO;
        for (NSNumber *k in r->surfaces)
            if (k.unsignedIntValue != mapping && r->surfaces[k]->video == s->video) elsewhere = YES;
        if (!elsewhere) atomic_store(&s->backs_off, 1);
    }
    /* The screen showing it lets go of it too: its texture is over pages the guest has given back, and the fence
       after this (afterGPU) waits only for frames already taken. Nothing is shown until the next swap; the layer
       keeps its last frame. on_screen 0 is what a snapshot saves for "nothing shown". */
    if (mapping == r->on_screen) {
        os_unfair_lock_lock(&r->screen_lock);
        r->screen_tex = nil;
        os_unfair_lock_unlock(&r->screen_lock);
        r->on_screen = 0;
    }
    if (r->late && r->late_mapping == mapping) r->late = NO; /* a delayed CPU present of it: no more */
    [r->surfaces removeObjectForKey:@(mapping)];
}

static void set_unmapped(HvReplay *r, uint32_t mapping, BOOL is) {
    if (mapping >= HV_PVG_MAPPING_IDS) return; /* none the mapper takes (it refuses them) */
    if (is) r->unmapped[mapping / 8] |= (uint8_t)(1u << (mapping % 8));
    else r->unmapped[mapping / 8] &= (uint8_t)~(1u << (mapping % 8));
}

void hv_pvg_metal_unmap(HvPvgMetal *m, uint32_t mapping) {
    HvReplay *r = (__bridge HvReplay *)m;
    dispatch_async(r->worker, ^{
        drop_surface(r, mapping, NO);
        set_unmapped(r, mapping, YES);
    });
}

void hv_pvg_metal_map(HvPvgMetal *m, uint32_t mapping) {
    HvReplay *r = (__bridge HvReplay *)m;
    dispatch_async(r->worker, ^{
        set_unmapped(r, mapping, NO);
        /* A surface still under the id: its unmap was passed over, so it is over the pages of the id's last mapping. The
           new one is made from the guest's descriptor as it is now, on its first use. */
        if (r->surfaces[@(mapping)]) {
            if (r->stale_dropped++ < 8) fprintf(stderr, "hengevm: pvg-metal: surface %u mapped again with no unmap: the old one dropped\n", mapping);
            drop_surface(r, mapping, YES);
        }
    });
}

void hv_pvg_metal_mapper_reset(HvPvgMetal *m) {
    HvReplay *r = (__bridge HvReplay *)m;
    dispatch_async(r->worker, ^{ memset(r->unmapped, 0, sizeof r->unmapped); }); /* what it passed over may have mapped them again */
}

void hv_pvg_metal_set_unmapped(HvPvgMetal *m, const uint8_t unmapped[HV_PVG_MAPPING_IDS / 8]) {
    HvReplay *r = (__bridge HvReplay *)m;
    NSData *bits = [NSData dataWithBytes:unmapped length:HV_PVG_MAPPING_IDS / 8]; /* as it is now: the mapper's goes on */
    dispatch_async(r->worker, ^{
        memcpy(r->unmapped, bits.bytes, sizeof r->unmapped);
        r->unmapped[0] &= (uint8_t)~1u; /* id 0 is none */
    });
}

/* A small picture of what the display shows, BGRA with no padding, for the app's home screen.
   Read straight from the surface's memory, nearest pixel: it may catch a frame being drawn, which
   does not matter at this size. -1 before anything has been shown, and after the surface shown is unmapped.
   An unmap between taking tex and reading it gives one thumbnail of stale pixels: tex keeps its buffer, over guest
   RAM that stays mapped, so the read is safe and the next thumbnail is right. */
int hv_pvg_metal_snapshot(HvPvgMetal *m, uint8_t *bgra, uint32_t w, uint32_t h) {
    if (!m || !w || !h) return -1;
    HvReplay *r = (__bridge HvReplay *)m;
    os_unfair_lock_lock(&r->screen_lock);
    id<MTLTexture> tex = r->screen_tex;
    BOOL half = r->screen_half;
    uint32_t W = r->screen_w, H = r->screen_h;
    os_unfair_lock_unlock(&r->screen_lock);
    id<MTLBuffer> buffer = tex.buffer;
    if (!buffer || !W || !H || buffer.storageMode == MTLStorageModePrivate) return -1;
    const uint8_t *pixels = (const uint8_t *)buffer.contents + tex.bufferOffset;
    size_t row = tex.bufferBytesPerRow;
    /* Four samples to a pixel: one makes text look broken at this size. */
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *out = bgra + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, out += 4) {
            uint32_t sum[3] = {0, 0, 0};
            for (uint32_t k = 0; k < 4; k++) {
                size_t sx = (2 * x + (k & 1)) * (uint64_t)W / (2 * w), sy = (2 * y + (k >> 1)) * (uint64_t)H / (2 * h);
                const uint8_t *src = pixels + sy * row;
                if (!half) {
                    for (int c = 0; c < 3; c++) sum[c] += src[sx * 4 + c];
                    continue;
                }
                const __fp16 *p = (const __fp16 *)(src + sx * 8);
                for (int c = 0; c < 3; c++) {
                    float v = hv_half_shown((float)p[c], hv_half_mode_get()); /* as the screen shows it */
                    sum[2 - c] += (uint32_t)(v * 255 + 0.5f);
                }
            }
            for (int c = 0; c < 3; c++) out[c] = (uint8_t)((sum[c] + 2) / 4);
            out[3] = 255;
        }
    }
    return 0;
}

int hv_pvg_metal_frame(HvPvgMetal *m, uint8_t *to, size_t cap, uint32_t *w, uint32_t *h) {
    if (!m) return -1;
    HvReplay *r = (__bridge HvReplay *)m;
    int rc = -1;
    os_unfair_lock_lock(&r->frame_lock);
    if (r->frame) {
        *w = r->frame_w, *h = r->frame_h;
        size_t need = (size_t)r->frame_w * r->frame_h * 4;
        rc = to && cap >= need ? 0 : 1;
        if (!rc) memcpy(to, r->frame, need);
    }
    os_unfair_lock_unlock(&r->frame_lock);
    return rc;
}

/* ---- Suspend lab (Phase 0): what of the Mac's graphics lives only on the iPad's GPU, and what reading it
   back costs. Measurement only: nothing the guest sees changes. ---- */

typedef struct { uint64_t n, bytes; } LabTally;
static void lab_add(LabTally *t, uint64_t bytes) { t->n++, t->bytes += bytes; }

/* Bytes a pixel, for plain formats; 0 for compressed and packed ones this test skips. */
static uint32_t lab_bpp(MTLPixelFormat f) {
    if (f == MTLPixelFormatDepth16Unorm) return 2;
    if (f == MTLPixelFormatDepth32Float) return 4;
    if (f == MTLPixelFormatStencil8) return 1;
    if (f >= 1 && f < 20) return 1;
    if (f >= 20 && f < 50) return 2;
    if (f >= 50 && f < 100) return 4;
    if (f >= 100 && f < 120) return 8;
    if (f >= 120 && f < 130) return 16;
    return 0;
}

void hv_pvg_metal_lab(HvPvgMetal *m, int readback, const char *when) {
    if (!m) return;
    HvReplay *r = (__bridge HvReplay *)m;
    NSString *label_s = [NSString stringWithUTF8String:when ? when : ""];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(r->worker, ^{
        @autoreleasepool {
            const char *label = label_s.UTF8String;
            double t0 = now_seconds();
            LabTally heaps = {0}, heaps_gpu = {0}, heaps_nopages = {0};
            LabTally gtex = {0}, gtex_gpu = {0}, gtex_unblit = {0};
            LabTally ptex = {0}, ptex_msaa = {0}, ptex_depth = {0}, ptex_rt = {0}, ptex_unknown = {0};
            LabTally in_heap = {0}, views = {0}, linear = {0}, buffers = {0}, mapped = {0}, code = {0}, other = {0};
            NSMutableArray<HvObject *> *gpu_heaps = [NSMutableArray new], *gpu_gtex = [NSMutableArray new], *gpu_ptex = [NSMutableArray new];
            for (uint32_t task = 0; task < HV_TASKS; task++) {
                HvObject *o;
                for (uint32_t at = 0; (o = objs_next(r->tasks[task], &at, NULL)); at++) {
                    id obj = o->object;
                    uint32_t type = o->entry[0] & 0xff;
                    if (!obj) continue;
                    if (type == OBJ_FUNCTION || type == OBJ_SERIALIZED) { lab_add(&code, 0); continue; }
                    if (o->heap_whole) {
                        uint64_t size = ((id<MTLHeap>)obj).size;
                        lab_add(&heaps, size);
                        if (!has_guest_pages(o)) lab_add(&heaps_nopages, size);
                        if (o->host_valid && (o->guest_stale || !has_guest_pages(o))) { lab_add(&heaps_gpu, size); [gpu_heaps addObject:o]; }
                        continue;
                    }
                    if (o->parent && o->parent->heap_whole) { lab_add(&in_heap, 0); continue; }
                    if (o->surface || type == OBJ_MAPPED_TEXTURE) { lab_add(&mapped, [obj conformsToProtocol:@protocol(MTLResource)] ? ((id<MTLResource>)obj).allocatedSize : 0); continue; }
                    if (o->parent) { lab_add(&views, 0); continue; }
                    if ([obj conformsToProtocol:@protocol(MTLTexture)]) {
                        id<MTLTexture> t = obj;
                        uint64_t size = t.allocatedSize;
                        if (t.buffer) { lab_add(&linear, 0); continue; }
                        if (o->guest_backed) {
                            lab_add(&gtex, size);
                            BOOL unblit = !o->tex_layout || t.sampleCount > 1 || t.isFramebufferOnly;
                            if (unblit) lab_add(&gtex_unblit, size);
                            if (o->host_valid && (o->guest_stale || !has_guest_pages(o))) { lab_add(&gtex_gpu, size); [gpu_gtex addObject:o]; }
                            continue;
                        }
                        lab_add(&ptex, size); /* private, with no guest memory at all: only ever on the GPU */
                        if (t.sampleCount > 1) lab_add(&ptex_msaa, size);
                        else if (t.pixelFormat >= MTLPixelFormatDepth16Unorm && t.pixelFormat <= 261) lab_add(&ptex_depth, size);
                        if (t.usage & MTLTextureUsageRenderTarget) lab_add(&ptex_rt, size);
                        if (t.sampleCount > 1 || !lab_bpp(t.pixelFormat) || t.isFramebufferOnly) lab_add(&ptex_unknown, size);
                        else [gpu_ptex addObject:o];
                        continue;
                    }
                    if ([obj conformsToProtocol:@protocol(MTLBuffer)]) {
                        id<MTLBuffer> b = obj;
                        if (b.storageMode == MTLStorageModePrivate) lab_add(&other, b.allocatedSize);
                        else lab_add(&buffers, b.length);
                        continue;
                    }
                    lab_add(&other, 0);
                }
            }
            double t1 = now_seconds();
#define MB(x) ((double)(x) / 1048576.0)
            fprintf(stderr, "lab: gpu %s: inventory in %.1f ms; device has %.0f MB allocated (working set %.0f MB)\n", label,
                    (t1 - t0) * 1e3, MB(r->device.currentAllocatedSize), MB(r->device.recommendedMaxWorkingSetSize));
            fprintf(stderr, "lab: gpu %s: heaps %llu = %.1f MB (GPU-only %llu = %.1f MB, no guest pages %llu = %.1f MB); in heaps %llu\n", label,
                    heaps.n, MB(heaps.bytes), heaps_gpu.n, MB(heaps_gpu.bytes), heaps_nopages.n, MB(heaps_nopages.bytes), in_heap.n);
            fprintf(stderr, "lab: gpu %s: guest textures %llu = %.1f MB (GPU-only %llu = %.1f MB, not blittable %llu = %.1f MB)\n", label,
                    gtex.n, MB(gtex.bytes), gtex_gpu.n, MB(gtex_gpu.bytes), gtex_unblit.n, MB(gtex_unblit.bytes));
            fprintf(stderr, "lab: gpu %s: GPU-only textures %llu = %.1f MB (multisample %llu = %.1f MB, depth/stencil %llu = %.1f MB, "
                    "render targets %llu = %.1f MB, unreadable here %llu = %.1f MB)\n", label,
                    ptex.n, MB(ptex.bytes), ptex_msaa.n, MB(ptex_msaa.bytes), ptex_depth.n, MB(ptex_depth.bytes), ptex_rt.n, MB(ptex_rt.bytes),
                    ptex_unknown.n, MB(ptex_unknown.bytes));
            fprintf(stderr, "lab: gpu %s: in guest memory: buffers %llu = %.1f MB, surfaces %llu = %.1f MB, linear %llu; views %llu; "
                    "functions/pipelines %llu; private buffers %llu = %.1f MB; surfaces kept %lu\n", label,
                    buffers.n, MB(buffers.bytes), mapped.n, MB(mapped.bytes), linear.n, views.n, code.n, other.n, MB(other.bytes),
                    (unsigned long)r->surfaces.count);
            if (readback) {
                /* Read every GPU-only byte into one scratch buffer, over and over: the cost a suspend pays. */
                const NSUInteger scratch_len = 64ull << 20;
                id<MTLBuffer> scratch = [r->device newBufferWithLength:scratch_len options:MTLResourceStorageModeShared];
                uint64_t moved = 0, skipped = 0;
                double gpu = 0, wall0 = now_seconds();
                NSString *error = nil;
                id<MTLBlitCommandEncoder> blit = nil;
                uint64_t in_cb = 0;
                __block id<MTLCommandBuffer> pending = nil;
                __block double gpu_sum = 0;
                __block NSString *err = nil;
                void (^flush)(void) = ^{
                    if (!pending) return;
                    if (err) { pending = nil; return; } /* after a failure or a buffer that never finished: no more */
                    [pending commit];
                    if (!cb_wait(r, pending, HV_PVG_SNAP_GPU_WAIT, NULL)) { err = @"a read back still running after its longest wait"; pending = nil; return; }
                    if (pending.status == MTLCommandBufferStatusError && !err) err = [NSString stringWithFormat:@"%ld %@", (long)pending.error.code, pending.error.localizedDescription];
                    if (pending.GPUEndTime > pending.GPUStartTime) gpu_sum += pending.GPUEndTime - pending.GPUStartTime;
                    pending = nil;
                };
                if (!scratch) error = @"no scratch buffer";
                else { /* at least one command buffer, so a GPU refused in the background shows even with nothing to read */
                    pending = [r->queue commandBuffer];
                    blit = [pending blitCommandEncoder];
                    [blit fillBuffer:scratch range:NSMakeRange(0, 16) value:0];
                }
                for (int pass = 0; scratch && pass < 3 && !err; pass++) {
                    NSArray<HvObject *> *list = pass == 0 ? gpu_heaps : pass == 1 ? gpu_gtex : gpu_ptex;
                    for (HvObject *o in list) {
                        if (!pending) { pending = [r->queue commandBuffer]; blit = [pending blitCommandEncoder]; in_cb = 0; }
                        if (pass == 0) {
                            for (NSUInteger at = 0; at < o->heap_whole.length; at += scratch_len) {
                                NSUInteger n = MIN(scratch_len, o->heap_whole.length - at);
                                [blit copyFromBuffer:o->heap_whole sourceOffset:at toBuffer:scratch destinationOffset:0 size:n];
                                moved += n, in_cb += n;
                            }
                        } else {
                            id<MTLTexture> t = o->object;
                            NSUInteger faces = t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 : 1;
                            NSUInteger slices = (t.textureType == MTLTextureType3D ? 1 : t.arrayLength) * faces;
                            uint32_t bpp = lab_bpp(t.pixelFormat);
                            for (NSUInteger s = 0; s < slices; s++)
                                for (NSUInteger lv = 0; lv < t.mipmapLevelCount; lv++) {
                                    NSUInteger w = MAX(1, t.width >> lv), h = MAX(1, t.height >> lv), d = MAX(1, t.depth >> lv);
                                    uint64_t row, image;
                                    if (pass == 1 && o->tex_layout && lv < o->tex_mips) {
                                        const uint8_t *mip = o->tex_layout + lv * 36;
                                        uint32_t bh = o->tex_block_h ? o->tex_block_h : 1;
                                        memcpy(&row, mip + 0x10, 8);
                                        image = row * ((h + bh - 1) / bh);
                                    } else if (bpp) {
                                        row = (uint64_t)w * bpp, image = row * h;
                                    } else { skipped += (uint64_t)w * h * d * 4; continue; }
                                    if (!row || image * d > scratch_len) { skipped += image * d; continue; }
                                    MTLBlitOption opt = t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
                                    [blit copyFromTexture:t sourceSlice:s sourceLevel:lv sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, d)
                                                 toBuffer:scratch destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:image options:opt];
                                    moved += image * d, in_cb += image * d;
                                }
                        }
                        if (in_cb >= (256ull << 20)) { [blit endEncoding]; blit = nil; flush(); }
                    }
                }
                if (pending) { [blit endEncoding]; flush(); }
                gpu = gpu_sum;
                if (err) error = err;
                double wall = now_seconds() - wall0;
                fprintf(stderr, "lab: gpu %s: read back %.1f MB in %.1f ms (GPU %.1f ms, %.0f MB/s), skipped %.1f MB%s%s\n", label, MB(moved),
                        wall * 1e3, gpu * 1e3, wall > 0 ? MB(moved) / wall : 0, MB(skipped), error ? "; FAILED: " : "", error ? error.UTF8String : "");
            }
#undef MB
            dispatch_semaphore_signal(done);
        }
    });
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC)))
        fprintf(stderr, "lab: gpu %s: still busy after 8 s; it reports when done\n", label_s.UTF8String);
}

/* ---- Suspend: what lives only on the GPU, saved with the Mac and put back after.

   Most of the Mac's graphics are in its own memory, which the snapshot keeps: buffers, surfaces (the
   screen itself), textures the guest gave pages to. The GPU's copies are made again from those the
   first time they are used. What is not is read back into the snapshot; nothing is written to the
   Mac's memory while saving, so a save that fails or is given up leaves the Mac exactly as it was:
   - heaps whose GPU copy is newer than their pages, or that lack some pages: their bytes;
   - textures with no guest memory (render targets, mostly), guest textures the GPU drew into since the
     guest last asked for them or that lack some pages, and textures made apart from a heap that could
     not hold them: their pixels, read through Metal (in no GPU's own layout);
   - buffers made apart from a heap: their bytes.
   Multisample, framebuffer-only, PVRTC and two-plane video textures cannot be read this way: they come
   back empty, and are counted.
   A process that restores has other GPU addresses and resource IDs for everything. What the old one told
   the guest of them (info 460...468) it tells again, wherever the guest still has the old answer. ---- */

enum { SNAP_PIXELS = 1, SNAP_BYTES = 2, SNAP_REBUILD = 3 };
#define PVGM_MAGIC 0x4d475650u /* "PVGM" */
#define PVGM_VERSION 2u
#define SNAP_WINDOW (64ull << 20) /* what is read back (or put back) at a time: the GPU may hold gigabytes, never all in memory at once */
#define SNAP_WRITE_GRACE_NS (120ull * 1000000000ull) /* once the GPU's work is done, the time the reading and writing may take */
#define PVGM_ALIGN 256u
#define PVGM_REC_MIN 32u     /* the smallest record's bytes: what a count is checked against */
#define PVGM_ANSWER_MIN 72u  /* the same for an answer */
#define PVGM_NAME_MAX 127u

static uint64_t snap_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); /* the clock the board's save deadline is on */
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Bytes per block of pixels in a part copied with this option, and the block's size in pixels (1 by 1
   but for compressed formats); 0 if it can't be copied */
static uint32_t snap_block(MTLPixelFormat f, MTLBlitOption opt, uint32_t *bw, uint32_t *bh) {
    *bw = *bh = 1;
    if (opt == MTLBlitOptionDepthFromDepthStencil) return f == MTLPixelFormatDepth32Float_Stencil8 ? 4 : 0;
    if (opt == MTLBlitOptionStencilFromDepthStencil) return f == MTLPixelFormatDepth32Float_Stencil8 ? 1 : 0;
    if (opt != MTLBlitOptionNone) return 0;
#define ASTC(w, h) case MTLPixelFormatASTC_##w##x##h##_sRGB: case MTLPixelFormatASTC_##w##x##h##_LDR: case MTLPixelFormatASTC_##w##x##h##_HDR: \
    *bw = w, *bh = h; return 16;
    switch (f) {
    case MTLPixelFormatDepth32Float_Stencil8: return 0; /* only as its two parts */
    case MTLPixelFormatBC1_RGBA: case MTLPixelFormatBC1_RGBA_sRGB: case MTLPixelFormatBC4_RUnorm: case MTLPixelFormatBC4_RSnorm:
    case MTLPixelFormatEAC_R11Unorm: case MTLPixelFormatEAC_R11Snorm: case MTLPixelFormatETC2_RGB8: case MTLPixelFormatETC2_RGB8_sRGB:
    case MTLPixelFormatETC2_RGB8A1: case MTLPixelFormatETC2_RGB8A1_sRGB:
        *bw = *bh = 4;
        return 8;
    case MTLPixelFormatBC2_RGBA: case MTLPixelFormatBC2_RGBA_sRGB: case MTLPixelFormatBC3_RGBA: case MTLPixelFormatBC3_RGBA_sRGB:
    case MTLPixelFormatBC5_RGUnorm: case MTLPixelFormatBC5_RGSnorm: case MTLPixelFormatBC6H_RGBFloat: case MTLPixelFormatBC6H_RGBUfloat:
    case MTLPixelFormatBC7_RGBAUnorm: case MTLPixelFormatBC7_RGBAUnorm_sRGB:
    case MTLPixelFormatEAC_RG11Unorm: case MTLPixelFormatEAC_RG11Snorm: case MTLPixelFormatEAC_RGBA8: case MTLPixelFormatEAC_RGBA8_sRGB:
        *bw = *bh = 4;
        return 16;
    ASTC(4, 4) ASTC(5, 4) ASTC(5, 5) ASTC(6, 5) ASTC(6, 6) ASTC(8, 5) ASTC(8, 6) ASTC(8, 8)
    ASTC(10, 5) ASTC(10, 6) ASTC(10, 8) ASTC(10, 10) ASTC(12, 10) ASTC(12, 12)
    default: break;
    }
#undef ASTC
    if (f == 552 || f == 553) return 8; /* BGRA10_XR */
    if (f == 554 || f == 555) return 4; /* BGR10_XR */
    return lab_bpp(f);
}

static uint64_t snap_row(uint32_t blocks, uint32_t bpb) {
    uint64_t a = bpb > 4 ? bpb : 4;
    return ((uint64_t)blocks * bpb + a - 1) / a * a;
}

typedef struct {
    uint32_t slice, level, w, h, d, opt, bw, bh;
    uint64_t row, image; /* bytes from one row of blocks to the next, and from one image (of a 3D texture) to the next */
    const uint8_t *bytes; /* restore: in the snapshot */
    uint64_t at; /* in the staging buffer, from the record's start */
} SnapPart;

/* The parts a texture's pixels are copied in, or 0 if they can't be: one per slice and level (two for depth and stencil) */
static uint32_t snap_parts(id<MTLTexture> t, SnapPart *out, uint32_t max, uint64_t *bytes) {
    if (t.sampleCount > 1 || t.isFramebufferOnly || t.storageMode == MTLStorageModeMemoryless) return 0;
    MTLBlitOption opts[2] = {MTLBlitOptionNone, MTLBlitOptionNone};
    uint32_t nopt = 1;
    if (t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
        opts[0] = MTLBlitOptionDepthFromDepthStencil, opts[1] = MTLBlitOptionStencilFromDepthStencil, nopt = 2;
    NSUInteger faces = t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 : 1;
    NSUInteger slices = (t.textureType == MTLTextureType3D ? 1 : t.arrayLength) * faces;
    uint32_t n = 0;
    uint64_t total = 0;
    for (NSUInteger s = 0; s < slices; s++)
        for (NSUInteger lv = 0; lv < t.mipmapLevelCount; lv++)
            for (uint32_t k = 0; k < nopt; k++) {
                uint32_t bw, bh, bpb = snap_block(t.pixelFormat, opts[k], &bw, &bh);
                if (!bpb || n >= max) return 0;
                SnapPart *p = &out[n++];
                memset(p, 0, sizeof *p);
                p->slice = (uint32_t)s, p->level = (uint32_t)lv, p->opt = (uint32_t)opts[k], p->bw = bw, p->bh = bh;
                p->w = (uint32_t)MAX(1, t.width >> lv), p->h = (uint32_t)MAX(1, t.height >> lv);
                p->d = (uint32_t)(t.textureType == MTLTextureType3D ? MAX(1, t.depth >> lv) : 1);
                p->row = snap_row((p->w + bw - 1) / bw, bpb), p->image = p->row * ((p->h + bh - 1) / bh);
                p->at = total;
                total += (p->image * p->d + PVGM_ALIGN - 1) / PVGM_ALIGN * PVGM_ALIGN;
            }
    *bytes = total;
    return n;
}

/* Made by itself where a heap could not hold it (makeHeapBuffer's and makeLinearTexture's fallbacks):
   its bytes are its own, not the heap's */
static BOOL snap_apart(HvObject *o) {
    return o->parent && o->parent->heap_whole && [o->object conformsToProtocol:@protocol(MTLResource)] && ((id<MTLResource>)o->object).heap == nil;
}

/* The buffer a SNAP_BYTES record is the bytes of */
static id<MTLBuffer> snap_bytes_of(HvObject *o) {
    if (o->heap_whole) return o->heap_whole;
    if (snap_apart(o) && [o->object conformsToProtocol:@protocol(MTLBuffer)]) return o->object;
    return nil;
}

/* Every page of its guest memory there, by the guest's table as it is now. Reads only. */
static BOOL snap_all_pages(HvReplay *r, uint32_t task, HvObject *o) {
    uint64_t n = (o->length + 0x3fff) >> 14;
    if (!o->vpage || !n) return NO;
    uint32_t *now = calloc(n, 4);
    if (!now) return NO;
    hv_pvg_lite_task_pages(r->lite, task, o->vpage, n, now);
    BOOL all = YES;
    for (uint64_t i = 0; i < n && all; i++) all = now[i] && hv_pvg_lite_page(r->lite, now[i]) != NULL;
    free(now);
    return all;
}

/* The GPU and the system's build, to know a snapshot from another */
static void snap_host_names(id<MTLDevice> dev, char device[PVGM_NAME_MAX + 1], char build[PVGM_NAME_MAX + 1]) {
    snprintf(device, PVGM_NAME_MAX + 1, "%s", dev.name.UTF8String ? dev.name.UTF8String : "");
    size_t n = PVGM_NAME_MAX;
    memset(build, 0, PVGM_NAME_MAX + 1);
    if (sysctlbyname("kern.osversion", build, &n, NULL, 0) != 0) build[0] = 0;
    build[PVGM_NAME_MAX] = 0;
}

#define SNAP_MAX_PARTS 4096u

typedef struct {
    uint32_t task, ref, entry[3], kind, host_valid, guest_stale;
    uint32_t nparts;
    SnapPart *parts; /* PIXELS */
    uint64_t len; /* BYTES */
    const uint8_t *bytes;
    uint64_t at; /* restore: in the staging buffer (unused now: each part goes through the window) */
    uint64_t span; /* save: its bytes, all parts aligned */
    __unsafe_unretained HvObject *o; /* save only: the tasks dictionary keeps it */
} SnapRec;

typedef struct {
    SnapAnswer a;
    uint32_t task;
    const uint8_t *rec;
} SnapParsedAnswer;

typedef struct {
    char device[PVGM_NAME_MAX + 1], build[PVGM_NAME_MAX + 1];
    uint32_t nrec, nans, lost, cursor_w, cursor_h, cursor_stride;
    int32_t hot_x, hot_y;
    uint8_t cursor_shown, ever_shown;
    uint64_t lost_bytes, on_screen, staging;
    const uint8_t *cursor;
    SnapRec *recs;
    SnapParsedAnswer *ans;
} SnapParsed;

static void snap_free(SnapParsed *s) {
    for (uint32_t i = 0; s->recs && i < s->nrec; i++) free(s->recs[i].parts);
    free(s->recs);
    free(s->ans);
    memset(s, 0, sizeof *s);
}

static const uint8_t *snap_take(HvPvgReader *r, uint64_t n) {
    if (r->bad || n > r->len - r->at) { r->bad = 1; return NULL; }
    const uint8_t *p = r->p + r->at;
    r->at += (size_t)n;
    return p;
}

static void snap_get_name(HvPvgReader *in, char out[PVGM_NAME_MAX + 1]) {
    uint32_t n = 0;
    hv_pvg_blob_get(in, &n, sizeof n);
    memset(out, 0, PVGM_NAME_MAX + 1);
    if (n > PVGM_NAME_MAX) { in->bad = 1; return; }
    const uint8_t *p = snap_take(in, n);
    if (p) memcpy(out, p, n);
    if (memchr(out, 0, n)) in->bad = 1; /* a name with a nul in it was not written here */
}

/* Reads (and checks) a saved GPU state; no Metal needed. 0, or -1 if it is damaged. */
static int snap_parse(const uint8_t *data, size_t len, SnapParsed *s) {
    memset(s, 0, sizeof *s);
    HvPvgReader in = {data, len, 0, 0};
#define G(f) hv_pvg_blob_get(&in, &(f), sizeof(f))
#define LEFT ((uint64_t)(in.len - in.at))
    uint32_t magic = 0, version = 0, has_cursor = 0;
    G(magic), G(version);
    if (in.bad || magic != PVGM_MAGIC || version != PVGM_VERSION) return -1;
    snap_get_name(&in, s->device);
    snap_get_name(&in, s->build);
    G(s->lost), G(s->lost_bytes), G(s->on_screen);
    G(has_cursor), G(s->cursor_w), G(s->cursor_h), G(s->cursor_stride), G(s->hot_x), G(s->hot_y), G(s->cursor_shown), G(s->ever_shown);
    if (in.bad || has_cursor > 1 || s->cursor_shown > 1 || s->ever_shown > 1) return -1;
    if (has_cursor) {
        if (!s->cursor_w || !s->cursor_h || s->cursor_w > 512 || s->cursor_h > 512 || s->cursor_stride < s->cursor_w * 4 || s->cursor_stride > 8192)
            return -1;
        s->cursor = snap_take(&in, (uint64_t)s->cursor_stride * s->cursor_h);
    }
    G(s->nrec);
    /* Every record takes some bytes: a count the rest can't hold is damage, not a reason to allocate */
    if (in.bad || s->nrec > (1u << 20) || s->nrec > LEFT / PVGM_REC_MIN) { s->nrec = 0; return -1; }
    s->recs = calloc(s->nrec ? s->nrec : 1, sizeof *s->recs);
    if (!s->recs) { s->nrec = 0; return -1; }
    uint64_t staging = 0;
    for (uint32_t i = 0; i < s->nrec && !in.bad; i++) {
        SnapRec *r = &s->recs[i];
        G(r->task), G(r->ref), G(r->entry), G(r->kind), G(r->host_valid), G(r->guest_stale);
        if (in.bad || r->task >= 512 || !r->ref || r->host_valid > 1 || r->guest_stale > 1) { in.bad = 1; break; }
        r->at = staging;
        if (r->kind == SNAP_PIXELS) {
            G(r->nparts);
            if (in.bad || !r->nparts || r->nparts > SNAP_MAX_PARTS || r->nparts > LEFT / 48 || !(r->parts = calloc(r->nparts, sizeof *r->parts))) { in.bad = 1; break; }
            for (uint32_t k = 0; k < r->nparts && !in.bad; k++) {
                SnapPart *p = &r->parts[k];
                G(p->slice), G(p->level), G(p->w), G(p->h), G(p->d), G(p->opt), G(p->bw), G(p->bh), G(p->row), G(p->image);
                if (in.bad || !p->w || !p->h || !p->d || p->w > 16384 || p->h > 16384 || p->d > 2048 || p->opt > 2 ||
                    !p->bw || !p->bh || p->bw > 12 || p->bh > 12 || p->row < (p->w + p->bw - 1) / p->bw || p->row > (1u << 20) ||
                    p->image != p->row * ((p->h + p->bh - 1) / p->bh)) { in.bad = 1; break; }
                uint64_t n = p->image * p->d;
                if (n > LEFT) { in.bad = 1; break; }
                p->bytes = snap_take(&in, n);
                p->at = staging - r->at;
                staging += (n + PVGM_ALIGN - 1) / PVGM_ALIGN * PVGM_ALIGN;
            }
        } else if (r->kind == SNAP_BYTES) {
            G(r->len);
            if (in.bad || !r->len || r->len > LEFT) { in.bad = 1; break; }
            r->bytes = snap_take(&in, r->len);
            staging += (r->len + PVGM_ALIGN - 1) / PVGM_ALIGN * PVGM_ALIGN;
        } else if (r->kind != SNAP_REBUILD) {
            in.bad = 1;
        }
    }
    G(s->nans);
    if (in.bad || s->nans > SNAP_MAX_ANSWERS || s->nans > LEFT / PVGM_ANSWER_MIN) { s->nans = 0; snap_free(s); return -1; }
    s->ans = calloc(s->nans ? s->nans : 1, sizeof *s->ans);
    if (!s->ans) { s->nans = 0; snap_free(s); return -1; }
    for (uint32_t i = 0; i < s->nans && !in.bad; i++) {
        SnapParsedAnswer *x = &s->ans[i];
        SnapAnswer *a = &x->a;
        G(x->task), G(a->tag), G(a->ref), G(a->bufref), G(a->rec_size), G(a->written), G(a->ref_entry), G(a->buf_entry), G(a->off), G(a->old);
        if (in.bad || x->task >= 512 || !info_is_address(a->tag) || !a->ref || !a->bufref || a->rec_size < info_rec_min(a->tag) || a->rec_size > SNAP_ANSWER_REC_MAX ||
            !a->written || a->written > sizeof a->old || a->off > (1ull << 40)) { in.bad = 1; break; }
        x->rec = snap_take(&in, a->rec_size);
        /* The question asks about what the answer says it does: re-told, it is written where it was checked */
        uint32_t qref = 0, qbuf = 0;
        uint64_t qoff = 0;
        if (x->rec) info_question(a->tag, x->rec, &qref, &qbuf, &qoff);
        if (in.bad || !x->rec || qref != a->ref || qbuf != a->bufref || qoff != a->off) { in.bad = 1; break; }
    }
#undef LEFT
#undef G
    s->staging = staging;
    if (in.bad || in.at != in.len) { snap_free(s); return -1; }
    return 0;
}

int hv_pvg_metal_check(const uint8_t *data, size_t len) {
    SnapParsed s;
    if (snap_parse(data, len, &s) != 0) return -1;
    snap_free(&s);
    return 0;
}

@interface HvSnapJob : NSObject {
@public
    dispatch_semaphore_t done;
    _Atomic int abandoned;
    os_unfair_lock lock; /* held while writing to sink; abandoning takes it, so nothing is written after */
    HvPvgSink sink;
    _Atomic int stage; /* SNAP_QUEUED, SNAP_FINISHING or SNAP_WRITING: what it was doing if it runs out of time */
    int rc;
    char why[160];
}
@end
enum { SNAP_QUEUED, SNAP_FINISHING, SNAP_WRITING };
@implementation HvSnapJob
@end

static void snap_why(HvSnapJob *job, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void snap_why(HvSnapJob *job, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(job->why, sizeof job->why, fmt, ap);
    va_end(ap);
    job->rc = -1;
}

/* Into the snapshot, unless the save was given up on (then it may be gone) */
static int snap_sink(HvSnapJob *job, const void *p, size_t n) {
    os_unfair_lock_lock(&job->lock);
    int rc = atomic_load(&job->abandoned) ? -1 : job->sink.write(job->sink.ctx, p, n);
    os_unfair_lock_unlock(&job->lock);
    return rc;
}

/* The save's bytes, in order: small ones (records' descriptions) gathered here, the GPU's read into a window of
   shared memory. When the window is full the GPU's reading is waited for and everything gathered so far goes to
   the snapshot in order; then the window is used again. However much the GPU holds, the save needs only the window. */
typedef struct {
    uint8_t from_stage;
    uint64_t off, len;
} SnapSeg;

typedef struct {
    HvReplay *__unsafe_unretained r;
    HvSnapJob *__unsafe_unretained job;
    id<MTLBuffer> stage;
    uint64_t window, used;
    id<MTLCommandBuffer> cb;
    id<MTLBlitCommandEncoder> blit;
    int submitted;
    HvPvgBlob small;
    SnapSeg *seg;
    uint32_t nseg, capseg;
    int bad;
} SnapW;

static void sw_seg(SnapW *w, uint8_t from_stage, uint64_t off, uint64_t len) {
    if (w->bad || !len) return;
    SnapSeg *last = w->nseg ? &w->seg[w->nseg - 1] : NULL;
    if (last && last->from_stage == from_stage && last->off + last->len == off) { last->len += len; return; }
    if (w->nseg == w->capseg) {
        uint32_t cap = w->capseg ? w->capseg * 2 : 256;
        SnapSeg *more = realloc(w->seg, cap * sizeof *more);
        if (!more) { snap_why(w->job, "not enough memory to save the screen"); w->bad = 1; return; }
        w->seg = more, w->capseg = cap;
    }
    w->seg[w->nseg++] = (SnapSeg){from_stage, off, len};
}

static void sw_put(SnapW *w, const void *p, size_t n) {
    if (w->bad) return;
    uint64_t off = w->small.len;
    hv_pvg_blob_put(&w->small, p, n);
    if (w->small.bad) { snap_why(w->job, "not enough memory to save the screen"); w->bad = 1; return; }
    sw_seg(w, 0, off, n);
}

/* The GPU's reading waited for, and everything gathered written out in order */
static void sw_flush(SnapW *w) {
    if (w->blit) {
        [w->blit endEncoding];
        [w->cb commit];
        BOOL done = cb_wait(w->r, w->cb, HV_PVG_SNAP_GPU_WAIT, &w->job->abandoned);
        w->submitted = 1;
        if (!w->bad && !done) { /* the window may still be being written: nothing of it goes out */
            atomic_fetch_add(&w->r->gpu_waits_given_up, 1);
            snap_why(w->job, atomic_load(&w->job->abandoned) ? "abandoned" : "the GPU took too long to read back the screen");
            w->bad = 1;
        } else if (!w->bad && w->cb.status != MTLCommandBufferStatusCompleted) {
            if (w->cb.error.code == MTLCommandBufferErrorNotPermitted)
                snap_why(w->job, "iOS doesn't let Henge use the GPU while it's in the background, so the screen couldn't be saved");
            else
                snap_why(w->job, "the GPU couldn't read back the screen (%s)", w->cb.error ? w->cb.error.localizedDescription.UTF8String : "no reason given");
            w->bad = 1;
        }
        w->blit = nil, w->cb = nil;
    }
    if (!w->bad && atomic_load(&w->job->abandoned)) { snap_why(w->job, "abandoned"); w->bad = 1; }
    const uint8_t *stage = w->stage ? w->stage.contents : NULL;
    for (uint32_t i = 0; !w->bad && i < w->nseg; i++) {
        SnapSeg *g = &w->seg[i];
        if (snap_sink(w->job, (g->from_stage ? stage : w->small.p) + g->off, (size_t)g->len) != 0) {
            snap_why(w->job, "the screen's state couldn't be written to storage (is the iPad's storage full?)");
            w->bad = 1;
        }
    }
    w->small.len = 0, w->nseg = 0, w->used = 0;
}

/* Room in the window for want bytes (want <= window): its offset there */
static uint64_t sw_room(SnapW *w, uint64_t want) {
    uint64_t at = (w->used + PVGM_ALIGN - 1) / PVGM_ALIGN * PVGM_ALIGN;
    if (at + want > w->window) sw_flush(w), at = 0;
    w->used = at + want;
    return at;
}

static id<MTLBlitCommandEncoder> sw_blit(SnapW *w) {
    if (!w->blit) {
        w->cb = [w->r->queue commandBuffer];
        w->blit = [w->cb blitCommandEncoder];
    }
    return w->blit;
}

static BOOL snap_is_depth(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatDepth16Unorm: case MTLPixelFormatDepth32Float: case MTLPixelFormatStencil8:
    case MTLPixelFormatDepth32Float_Stencil8: case MTLPixelFormatX32_Stencil8:
        return YES;
    default: return NO;
    }
}

/* A texture part's bytes into the save, through the window: whole if it fits, else in bands of rows.
   Depth and stencil are copied only whole: one bigger than the window goes through a buffer of its own. */
static void sw_part(SnapW *w, id<MTLTexture> t, const SnapPart *p) {
    uint64_t total = p->image * p->d;
    if (total > w->window && snap_is_depth(t.pixelFormat)) {
        sw_flush(w);
        if (w->bad) return;
        id<MTLBuffer> own = [w->r->device newBufferWithLength:(NSUInteger)total options:MTLResourceStorageModeShared];
        if (!own) { snap_why(w->job, "not enough memory to save the screen"); w->bad = 1; return; }
        id<MTLCommandBuffer> cb = [w->r->queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:t sourceSlice:p->slice sourceLevel:p->level sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(p->w, p->h, p->d) toBuffer:own destinationOffset:0
       destinationBytesPerRow:(NSUInteger)p->row destinationBytesPerImage:(NSUInteger)p->image options:(MTLBlitOption)p->opt];
        [blit endEncoding];
        [cb commit];
        BOOL done = cb_wait(w->r, cb, HV_PVG_SNAP_GPU_WAIT, &w->job->abandoned);
        w->submitted = 1;
        if (!done) {
            atomic_fetch_add(&w->r->gpu_waits_given_up, 1);
            snap_why(w->job, atomic_load(&w->job->abandoned) ? "abandoned" : "the GPU took too long to read back the screen");
            w->bad = 1;
        } else if (cb.status != MTLCommandBufferStatusCompleted) {
            snap_why(w->job, "the GPU couldn't read back the screen (%s)", cb.error ? cb.error.localizedDescription.UTF8String : "no reason given");
            w->bad = 1;
        } else if (snap_sink(w->job, own.contents, (size_t)total) != 0) {
            snap_why(w->job, "the screen's state couldn't be written to storage (is the iPad's storage full?)");
            w->bad = 1;
        }
        return;
    }
    if (total <= w->window) {
        uint64_t at = sw_room(w, total);
        if (w->bad) return;
        [sw_blit(w) copyFromTexture:t sourceSlice:p->slice sourceLevel:p->level sourceOrigin:MTLOriginMake(0, 0, 0)
                         sourceSize:MTLSizeMake(p->w, p->h, p->d) toBuffer:w->stage destinationOffset:(NSUInteger)at
             destinationBytesPerRow:(NSUInteger)p->row destinationBytesPerImage:(NSUInteger)p->image options:(MTLBlitOption)p->opt];
        sw_seg(w, 1, at, total);
        return;
    }
    uint64_t rows = p->image / p->row, band = w->window / p->row; /* rows of blocks */
    if (!band) band = 1;
    for (uint32_t z = 0; z < p->d && !w->bad; z++)
        for (uint64_t y = 0; y < rows && !w->bad; y += band) {
            uint64_t nb = rows - y < band ? rows - y : band;
            uint64_t py = y * p->bh, ph = nb * p->bh;
            if (py + ph > p->h) ph = p->h - py;
            uint64_t at = sw_room(w, nb * p->row);
            if (w->bad) return;
            [sw_blit(w) copyFromTexture:t sourceSlice:p->slice sourceLevel:p->level sourceOrigin:MTLOriginMake(0, (NSUInteger)py, z)
                             sourceSize:MTLSizeMake(p->w, (NSUInteger)ph, 1) toBuffer:w->stage destinationOffset:(NSUInteger)at
                 destinationBytesPerRow:(NSUInteger)p->row destinationBytesPerImage:(NSUInteger)(nb * p->row) options:(MTLBlitOption)p->opt];
            sw_seg(w, 1, at, nb * p->row);
        }
}

/* A buffer's bytes into the save, through the window */
static void sw_bytes(SnapW *w, id<MTLBuffer> b, uint64_t len) {
    for (uint64_t k = 0; k < len && !w->bad; ) {
        uint64_t n = len - k < w->window ? len - k : w->window;
        uint64_t at = sw_room(w, n);
        if (w->bad) return;
        [sw_blit(w) copyFromBuffer:b sourceOffset:(NSUInteger)k toBuffer:w->stage destinationOffset:(NSUInteger)at size:(NSUInteger)n];
        sw_seg(w, 1, at, n);
        k += n;
    }
}

/* On the worker, after everything handed to it before: nothing new comes, for the CPUs are stopped.
   Reads only: the guest's memory and what the GPU holds are as they were, whatever happens. */
static void snap_save_on_worker(HvReplay *r, HvSnapJob *job, uint64_t deadline_ns) {
    /* All the GPU work under way finishes, and the guest has its stamps */
    atomic_store(&job->stage, SNAP_FINISHING);
    while (r->last_cb && r->last_cb.status < MTLCommandBufferStatusCompleted) { /* no longer than the deadline: a hung one ends only at the GPU's watchdog */
        if (snap_mono_ns() > deadline_ns || atomic_load(&job->abandoned)) { snap_why(job, "the Mac's graphics work didn't finish in time"); return; }
        usleep(200);
    }
    while (atomic_load(&r->inflight) != 0) {
        if (snap_mono_ns() > deadline_ns || atomic_load(&job->abandoned)) { snap_why(job, "the Mac's graphics work didn't finish in time"); return; }
        usleep(200);
    }
    if (snap_mono_ns() > deadline_ns) { snap_why(job, "the Mac's graphics work didn't finish in time"); return; }
    if (r->answers_lost) { snap_why(job, "the Mac's graphics are in a state Henge can't save (resume, then restart the Mac to save again)"); return; }
    atomic_store(&job->stage, SNAP_WRITING);
    uint32_t cap = 64, n = 0, lost = 0;
    uint64_t lost_bytes = 0, staging = 0;
    SnapRec *recs = calloc(cap, sizeof *recs);
    SnapPart *parts = malloc(SNAP_MAX_PARTS * sizeof *parts); /* too big for a dispatch thread's stack */
    if (!recs || !parts) { snap_why(job, "not enough memory to save the screen"); goto out; }
#define LOST(obj) (lost++, lost_bytes += [(obj) conformsToProtocol:@protocol(MTLResource)] ? ((id<MTLResource>)(obj)).allocatedSize : 0)
    for (uint32_t task = 0; task < HV_TASKS; task++) {
        HvObject *o;
        uint32_t ref = 0;
        for (uint32_t at = 0; (o = objs_next(r->tasks[task], &at, &ref)); at++) {
            id obj = o->object;
            uint32_t type = o->entry[0] & 0xff, kind = 0, nparts = 0;
            uint64_t bytes = 0;
            if (!obj || type == OBJ_FUNCTION || type == OBJ_SERIALIZED) continue;
            BOOL apart = snap_apart(o);
            if (type == OBJ_MAPPED_TEXTURE) {
                kind = SNAP_REBUILD; /* made again at once: the screen is shown from its surface */
            } else if (o->heap_whole) {
                /* Which copy is newest: see exec. Pages newest: made again from them when next used, as at first */
                if (!o->host_valid && !o->guest_stale) continue;
                if (o->host_valid && !o->guest_stale && snap_all_pages(r, task, o))
                    kind = SNAP_REBUILD; /* the same in its pages: made again from them at once, for what is in it */
                else
                    kind = SNAP_BYTES, bytes = o->heap_whole.length;
            } else if (apart && [obj conformsToProtocol:@protocol(MTLBuffer)]) {
                kind = SNAP_BYTES, bytes = ((id<MTLBuffer>)obj).length;
            } else if ((o->parent && !apart) || o->surface || ![obj conformsToProtocol:@protocol(MTLTexture)] || ((id<MTLTexture>)obj).buffer) {
                continue; /* views, things in heaps, buffers, linear textures: made again from the guest's memory or what they're made from */
            } else {
                id<MTLTexture> t = obj;
                if (o->guest_backed) {
                    if (!o->host_valid && !o->guest_stale) continue;
                    if (o->host_valid && !o->guest_stale && snap_all_pages(r, task, o)) continue;
                }
                nparts = snap_parts(t, parts, SNAP_MAX_PARTS, &bytes);
                if (!nparts) {
                    if (t.storageMode != MTLStorageModeMemoryless) LOST(obj); /* memoryless ones hold nothing between passes */
                    continue;
                }
                kind = SNAP_PIXELS;
            }
            if (n == cap) {
                SnapRec *more = realloc(recs, (size_t)cap * 2 * sizeof *recs);
                if (!more) { snap_why(job, "not enough memory to save the screen"); goto out; }
                recs = more, cap *= 2;
                memset(recs + n, 0, (size_t)(cap - n) * sizeof *recs);
            }
            SnapRec *rec = &recs[n++];
            rec->task = task, rec->ref = ref, rec->kind = kind;
            memcpy(rec->entry, o->entry, sizeof rec->entry);
            rec->host_valid = o->host_valid, rec->guest_stale = o->guest_stale;
            rec->o = o;
            rec->at = staging;
            if (nparts) {
                rec->nparts = nparts;
                rec->parts = malloc(nparts * sizeof *parts);
                if (!rec->parts) { snap_why(job, "not enough memory to save the screen"); goto out; }
                memcpy(rec->parts, parts, nparts * sizeof *parts);
            }
            if (kind == SNAP_BYTES) rec->len = bytes;
            rec->span = (bytes + PVGM_ALIGN - 1) / PVGM_ALIGN * PVGM_ALIGN;
            staging += rec->span; /* only to size the window: it is read back a window at a time */
        }
    }
#undef LOST
    if (atomic_load(&job->abandoned)) { snap_why(job, "abandoned"); goto out; }
    {
        /* However much it is, it must fit: a storage filled to the brim mid-save helps no one */
        char room_why[160] = "";
        os_unfair_lock_lock(&job->lock);
        int room = atomic_load(&job->abandoned) || !job->sink.room ? 0 : job->sink.room(job->sink.ctx, staging, room_why, sizeof room_why);
        os_unfair_lock_unlock(&job->lock);
        if (room != 0) { snap_why(job, "%s", room_why); goto out; }
    }
    {
        SnapW w = {0};
        w.r = r, w.job = job;
        w.window = staging < SNAP_WINDOW ? staging : SNAP_WINDOW;
        if (w.window) {
            w.stage = [r->device newBufferWithLength:(NSUInteger)w.window options:MTLResourceStorageModeShared];
            if (!w.stage) { snap_why(job, "not enough memory to save the screen"); goto out; }
        }
        if (lost) fprintf(stderr, "hengevm: snapshot: %u GPU textures (%.1f MB) can't be read back and come back empty\n", lost, lost_bytes / 1048576.0);

#define P(f) sw_put(&w, &(f), sizeof(f))
        uint32_t magic = PVGM_MAGIC, version = PVGM_VERSION, has_cursor = 0, cw = 0, ch = 0, cs = 0;
        int32_t hx = 0, hy = 0;
        uint8_t shown = 0, ever = (uint8_t)(atomic_load(&r->cursor_ever_shown) != 0);
        uint8_t *cursor = NULL;
        char device[PVGM_NAME_MAX + 1], build[PVGM_NAME_MAX + 1];
        snap_host_names(r->device, device, build);
        os_unfair_lock_lock(&r->screen_lock);
        if (r->cursor) {
            has_cursor = 1, cw = r->cursor_w, ch = r->cursor_h, cs = r->cursor_stride, hx = r->cursor_hot_x, hy = r->cursor_hot_y;
            cursor = malloc((size_t)cs * ch);
            if (cursor) memcpy(cursor, r->cursor, (size_t)cs * ch);
        }
        shown = r->cursor_shown ? 1 : 0;
        os_unfair_lock_unlock(&r->screen_lock);
        if (has_cursor && !cursor) { snap_why(job, "not enough memory to save the screen"); goto out_w; }
        uint64_t on_screen = r->on_screen;
        P(magic), P(version);
        uint32_t dn = (uint32_t)strnlen(device, PVGM_NAME_MAX), bn = (uint32_t)strnlen(build, PVGM_NAME_MAX);
        P(dn), sw_put(&w, device, dn);
        P(bn), sw_put(&w, build, bn);
        P(lost), P(lost_bytes), P(on_screen);
        P(has_cursor), P(cw), P(ch), P(cs), P(hx), P(hy), P(shown), P(ever);
        if (has_cursor) sw_put(&w, cursor, (size_t)cs * ch);
        free(cursor);
        P(n);
        for (uint32_t i = 0; i < n && !w.bad; i++) {
            SnapRec *rec = &recs[i];
            P(rec->task), P(rec->ref), P(rec->entry), P(rec->kind), P(rec->host_valid), P(rec->guest_stale);
            if (rec->kind == SNAP_PIXELS) {
                P(rec->nparts);
                for (uint32_t k = 0; k < rec->nparts && !w.bad; k++) {
                    SnapPart *p = &rec->parts[k];
                    P(p->slice), P(p->level), P(p->w), P(p->h), P(p->d), P(p->opt), P(p->bw), P(p->bh), P(p->row), P(p->image);
                    sw_part(&w, rec->o->object, p);
                }
            } else if (rec->kind == SNAP_BYTES) {
                P(rec->len);
                sw_bytes(&w, snap_bytes_of(rec->o), rec->len);
            }
        }
        uint32_t nans = 0;
        for (NSNumber *task in r->answers) nans += (uint32_t)r->answers[task].count;
        P(nans);
        for (NSNumber *task in r->answers) {
            uint32_t t = task.unsignedIntValue;
            for (NSData *v in r->answers[task].allValues) {
                const SnapAnswer *a = v.bytes;
                P(t), P(a->tag), P(a->ref), P(a->bufref), P(a->rec_size), P(a->written), P(a->ref_entry), P(a->buf_entry), P(a->off), P(a->old);
                sw_put(&w, (const uint8_t *)(a + 1), a->rec_size);
            }
        }
#undef P
        /* Always one command buffer, so a GPU refused to an app in the background says so even with nothing to read */
        if (!w.submitted && !w.blit && !w.bad) sw_blit(&w);
        sw_flush(&w);
    out_w:
        free(w.small.p);
        free(w.seg);
    }
out:
    for (uint32_t i = 0; recs && i < n; i++) free(recs[i].parts);
    free(recs);
    free(parts);
}

int hv_pvg_metal_save(HvPvgMetal *m, const HvPvgSink *out, uint64_t deadline_ns, char *why, size_t why_len) {
    HvReplay *r = (__bridge HvReplay *)m;
    HvSnapJob *job = [HvSnapJob new];
    job->done = dispatch_semaphore_create(0);
    job->lock = OS_UNFAIR_LOCK_INIT;
    job->sink = *out;
    dispatch_async(r->worker, ^{
        @autoreleasepool {
            if (!atomic_load(&job->abandoned)) snap_save_on_worker(r, job, deadline_ns);
            else snap_why(job, "abandoned");
            dispatch_semaphore_signal(job->done);
        }
    });
    /* The deadline is for the GPU's work to finish: a save still waiting for it then is given up at once. Reading
       back and writing out what it holds may take longer when it holds a lot, but not without end. */
    uint64_t now = snap_mono_ns(), until = deadline_ns + SNAP_WRITE_GRACE_NS;
    int64_t first = deadline_ns > now ? (int64_t)(deadline_ns - now) : 0;
    long late = dispatch_semaphore_wait(job->done, dispatch_time(DISPATCH_TIME_NOW, first));
    if (late && atomic_load(&job->stage) == SNAP_WRITING) {
        now = snap_mono_ns();
        late = dispatch_semaphore_wait(job->done, dispatch_time(DISPATCH_TIME_NOW, until > now ? (int64_t)(until - now) : 0));
    }
    if (late) {
        os_unfair_lock_lock(&job->lock); /* any write under way finishes; none after */
        atomic_store(&job->abandoned, 1);
        os_unfair_lock_unlock(&job->lock);
        int stage = atomic_load(&job->stage);
        if (why && why_len)
            snprintf(why, why_len, "%s", stage == SNAP_WRITING ? "saving what's on the Mac's screen took too long"
                                         : stage == SNAP_FINISHING ? "the Mac's graphics work didn't finish in time"
                                                                   : "the Mac's graphics were busy and couldn't be saved in time");
        return -1;
    }
    if (job->rc) {
        if (why && why_len) snprintf(why, why_len, "%s", job->why);
        return -1;
    }
    return 0;
}

/* The guest's object table says the same of ref as when the answer was given */
static BOOL snap_entry_is(HvReplay *r, uint32_t task, uint32_t ref, const uint32_t want[3]) {
    uint64_t list = hv_pvg_lite_task_objects(r->lite, task);
    uint32_t e[3];
    return ref && list && !hv_pvg_lite_task_read(r->lite, task, list + (uint64_t)ref * 12, e, 12) && !memcmp(e, want, 12);
}

/* Tells the guest again the addresses and resource IDs it was told, as this process has them: only where
   the old answer is still there, untouched, and of the same objects. Returns how many. */
static uint32_t snap_reanswer(HvReplay *r, const SnapParsed *s, uint32_t *stale) {
    uint32_t told = 0;
    for (uint32_t i = 0; i < s->nans; i++) {
        const SnapParsedAnswer *x = &s->ans[i];
        const SnapAnswer *a = &x->a;
        if (!snap_entry_is(r, x->task, a->ref, a->ref_entry) || !snap_entry_is(r, x->task, a->bufref, a->buf_entry)) { (*stale)++; continue; }
        HvObject *to = [r object:a->bufref task:x->task];
        if (!to || !refresh_pages(r, x->task, to) || a->off + 28 > to->length || memcmp(to->mapped + a->off, a->old, a->written)) { (*stale)++; continue; }
        [r info:a->tag rec:(Rec){x->rec, a->rec_size} task:x->task];
        told++;
    }
    return told;
}

/* Putting the GPU's state back a window at a time: what is copied in waits for the GPU to take it before the
   window is used again. The last command buffer is left for the caller to queue ahead of the guest's work. */
typedef struct {
    HvReplay *__unsafe_unretained r;
    id<MTLBuffer> stage;
    uint64_t window, used;
    id<MTLCommandBuffer> cb;
    id<MTLBlitCommandEncoder> blit;
    int failed;
    int stuck; /* a window's buffer not done in HV_PVG_SNAP_GPU_WAIT: the GPU may read the window still, so nothing more
                  is copied into it (what is left starts empty) */
} SnapRW;

static uint64_t rw_room(SnapRW *w, uint64_t want) {
    uint64_t at = (w->used + PVGM_ALIGN - 1) / PVGM_ALIGN * PVGM_ALIGN;
    if (at + want > w->window) {
        if (w->blit) [w->blit endEncoding];
        w->blit = nil;
        [w->cb commit];
        if (!cb_wait(w->r, w->cb, HV_PVG_SNAP_GPU_WAIT, NULL)) {
            atomic_fetch_add(&w->r->gpu_waits_given_up, 1);
            w->stuck = w->failed = 1;
        } else if (w->cb.status != MTLCommandBufferStatusCompleted) w->failed = 1;
        w->cb = [w->r->queue commandBuffer];
        at = 0;
    }
    w->used = at + want;
    return at;
}

static id<MTLBlitCommandEncoder> rw_blit(SnapRW *w) {
    if (!w->blit) w->blit = [w->cb blitCommandEncoder];
    return w->blit;
}

/* NO once the GPU is stuck (SnapRW's stuck): not all of it got in */
static BOOL rw_bytes(SnapRW *w, const uint8_t *from, uint64_t len, id<MTLBuffer> to) {
    for (uint64_t k = 0; k < len; ) {
        uint64_t n = len - k < w->window ? len - k : w->window;
        uint64_t at = rw_room(w, n);
        if (w->stuck) return NO;
        memcpy((uint8_t *)w->stage.contents + at, from + k, (size_t)n);
        [rw_blit(w) copyFromBuffer:w->stage sourceOffset:(NSUInteger)at toBuffer:to destinationOffset:(NSUInteger)k size:(NSUInteger)n];
        k += n;
    }
    return YES;
}

/* A part's bytes into the texture: whole if it fits the window, else in bands of rows (depth and stencil
   only whole: through a buffer of their own). Returns NO if there was no memory for that, or the GPU is stuck (not
   all of it got in). */
static BOOL rw_part(SnapRW *w, id<MTLTexture> t, const SnapPart *p) {
    uint64_t total = p->image * p->d;
    if (total > w->window && snap_is_depth(t.pixelFormat)) {
        id<MTLBuffer> own = [w->r->device newBufferWithLength:(NSUInteger)total options:MTLResourceStorageModeShared];
        if (!own) return NO;
        memcpy(own.contents, p->bytes, (size_t)total);
        [rw_blit(w) copyFromBuffer:own sourceOffset:0 sourceBytesPerRow:(NSUInteger)p->row sourceBytesPerImage:(NSUInteger)p->image
                        sourceSize:MTLSizeMake(p->w, p->h, p->d) toTexture:t destinationSlice:p->slice destinationLevel:p->level
                 destinationOrigin:MTLOriginMake(0, 0, 0) options:(MTLBlitOption)p->opt];
        return YES; /* the command buffer keeps it until it is done */
    }
    if (total <= w->window) {
        uint64_t at = rw_room(w, total);
        if (w->stuck) return NO;
        memcpy((uint8_t *)w->stage.contents + at, p->bytes, (size_t)total);
        [rw_blit(w) copyFromBuffer:w->stage sourceOffset:(NSUInteger)at sourceBytesPerRow:(NSUInteger)p->row sourceBytesPerImage:(NSUInteger)p->image
                        sourceSize:MTLSizeMake(p->w, p->h, p->d) toTexture:t destinationSlice:p->slice destinationLevel:p->level
                 destinationOrigin:MTLOriginMake(0, 0, 0) options:(MTLBlitOption)p->opt];
        return YES;
    }
    uint64_t rows = p->image / p->row, band = w->window / p->row;
    if (!band) band = 1;
    const uint8_t *from = p->bytes;
    for (uint32_t z = 0; z < p->d; z++)
        for (uint64_t y = 0; y < rows; y += band) {
            uint64_t nb = rows - y < band ? rows - y : band;
            uint64_t py = y * p->bh, ph = nb * p->bh;
            if (py + ph > p->h) ph = p->h - py;
            uint64_t at = rw_room(w, nb * p->row);
            if (w->stuck) return NO;
            memcpy((uint8_t *)w->stage.contents + at, from, (size_t)(nb * p->row));
            from += nb * p->row;
            [rw_blit(w) copyFromBuffer:w->stage sourceOffset:(NSUInteger)at sourceBytesPerRow:(NSUInteger)p->row sourceBytesPerImage:(NSUInteger)(nb * p->row)
                            sourceSize:MTLSizeMake(p->w, (NSUInteger)ph, 1) toTexture:t destinationSlice:p->slice destinationLevel:p->level
                     destinationOrigin:MTLOriginMake(0, (NSUInteger)py, z) options:(MTLBlitOption)p->opt];
        }
    return YES;
}

/* On the worker. Takes bytes, and releases them. */
static void snap_restore_on_worker(HvReplay *r, HvPvgBytes bytes) {
    double t0 = now_seconds();
    SnapParsed s;
    if (snap_parse(bytes.p, bytes.len, &s) != 0) {
        fprintf(stderr, "hengevm: snapshot: the GPU's state is damaged: the screen starts blank\n");
        hv_pvg_bytes_release(&bytes);
        return;
    }
    char device[PVGM_NAME_MAX + 1], build[PVGM_NAME_MAX + 1];
    snap_host_names(r->device, device, build);
    /* Another GPU, or another system under it, may lay textures in a heap out otherwise: heaps' bytes are
       left out then (what is in them starts empty), the rest is in no GPU's own layout */
    BOOL same_gpu = !strcmp(device, s.device) && !strcmp(build, s.build);
    if (!same_gpu)
        fprintf(stderr, "hengevm: snapshot: saved on %s (%s), now %s (%s): heaps start empty\n", s.device, s.build, device, build);
    if (s.cursor) {
        uint8_t *bits = malloc((size_t)s.cursor_stride * s.cursor_h);
        MTLTextureDescriptor *ct = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:s.cursor_w height:s.cursor_h mipmapped:NO];
        ct.storageMode = MTLStorageModeShared;
        id<MTLTexture> glyph = bits ? [r->device newTextureWithDescriptor:ct] : nil;
        if (glyph) {
            memcpy(bits, s.cursor, (size_t)s.cursor_stride * s.cursor_h);
            [glyph replaceRegion:MTLRegionMake2D(0, 0, s.cursor_w, s.cursor_h) mipmapLevel:0 withBytes:bits bytesPerRow:s.cursor_stride];
            os_unfair_lock_lock(&r->screen_lock);
            free(r->cursor);
            r->cursor = bits, r->cursor_w = s.cursor_w, r->cursor_h = s.cursor_h, r->cursor_stride = s.cursor_stride;
            r->cursor_hot_x = s.hot_x, r->cursor_hot_y = s.hot_y, r->cursor_tex = glyph;
            os_unfair_lock_unlock(&r->screen_lock);
        } else {
            free(bits);
        }
    }
    os_unfair_lock_lock(&r->screen_lock);
    r->cursor_shown = s.cursor_shown;
    os_unfair_lock_unlock(&r->screen_lock);
    if (s.ever_shown) atomic_store(&r->cursor_ever_shown, 1);

    SnapRW w = {0};
    w.r = r;
    w.window = s.staging < SNAP_WINDOW ? s.staging : SNAP_WINDOW;
    w.stage = w.window ? [r->device newBufferWithLength:(NSUInteger)w.window options:MTLResourceStorageModeShared] : nil;
    id<MTLBuffer> stage = w.stage;
    if (s.staging && !stage) fprintf(stderr, "hengevm: snapshot: no memory for the GPU's saved state: what only it held starts empty\n");
    w.cb = [r->queue commandBuffer];
    uint32_t made = 0, skipped = 0, why_skip[4] = {0};
    SnapPart *want = malloc(SNAP_MAX_PARTS * sizeof *want);
    for (uint32_t i = 0; i < s.nrec; i++) {
        SnapRec *rec = &s.recs[i];
        /* Only into what the guest's table still says is the same object; made now if it wasn't */
        if (!hv_pvg_lite_task_objects(r->lite, rec->task)) { skipped++, why_skip[0]++; continue; }
        if (!snap_entry_is(r, rec->task, rec->ref, rec->entry)) { skipped++, why_skip[1]++; continue; }
        HvObject *o = [r object:rec->ref task:rec->task];
        if (!o || !o->object || memcmp(o->entry, rec->entry, sizeof rec->entry)) { skipped++, why_skip[2]++; continue; }
        if (rec->kind == SNAP_BYTES) {
            id<MTLBuffer> to = snap_bytes_of(o);
            if (!stage || !to || to.length != rec->len || (o->heap_whole && !same_gpu)) { skipped++, why_skip[3]++; continue; }
            if (!rw_bytes(&w, rec->bytes, rec->len, to)) { skipped++, why_skip[3]++; continue; } /* so not marked as the newest */
        } else if (rec->kind == SNAP_PIXELS) {
            /* Only into a texture just like the one saved: the same parts, in the same layout */
            if (!stage || !want || ![o->object conformsToProtocol:@protocol(MTLTexture)] || (o->parent && !snap_apart(o)) || o->surface) { skipped++, why_skip[3]++; continue; }
            uint64_t bytes = 0;
            uint32_t np = snap_parts(o->object, want, SNAP_MAX_PARTS, &bytes);
            BOOL same = np == rec->nparts;
            for (uint32_t k = 0; same && k < np; k++) {
                SnapPart *a = &want[k], *b = &rec->parts[k];
                same = a->slice == b->slice && a->level == b->level && a->w == b->w && a->h == b->h && a->d == b->d && a->opt == b->opt &&
                       a->bw == b->bw && a->bh == b->bh && a->row == b->row && a->image == b->image && a->at == b->at;
            }
            if (!same) { skipped++, why_skip[3]++; continue; }
            BOOL all = YES;
            for (uint32_t k = 0; k < np && all; k++) all = rw_part(&w, o->object, &rec->parts[k]);
            if (!all) { skipped++, why_skip[3]++; continue; } /* what got in is no worse than empty, and not marked as the newest */
        } else if (o->heap_whole && rec->host_valid) {
            id<MTLBlitCommandEncoder> blit = w.blit;
            [r page:o toGuest:NO task:rec->task blit:&blit commandBuffer:w.cb]; /* its pages have the newest bytes */
            w.blit = blit;
        }
        o->host_valid = rec->host_valid, o->guest_stale = rec->guest_stale;
        made++;
    }
    free(want);
    [w.blit endEncoding];
    [r commit:w.cb channel:0 stamp:0 complete:NO redraw:NO]; /* ahead of the guest's next work on the same queue */
    if (w.failed) fprintf(stderr, "hengevm: snapshot: the GPU didn't take some of its saved state: parts of the screen may start empty\n");
    uint32_t stale = 0, told = snap_reanswer(r, &s, &stale);
    r->on_screen = s.on_screen;
    fprintf(stderr, "hengevm: snapshot: GPU state back in %.0f ms: %u objects, %u no longer match and start empty; %u addresses told the guest again, %u not (no longer there)\n",
            (now_seconds() - t0) * 1000, made, skipped, told, stale);
    if (skipped)
        fprintf(stderr, "hengevm: snapshot: of those that didn't match: %u in a task the guest has no object table for, %u changed in the guest's table, "
                "%u not made again here, %u of another shape or size (or no memory, or the GPU stuck)\n", why_skip[0], why_skip[1], why_skip[2], why_skip[3]);
    if (s.on_screen && !r->surfaces[@(s.on_screen)])
        fprintf(stderr, "hengevm: snapshot: the frame on the screen when saved (surface %llu) isn't there: the screen waits for macOS to draw\n", (unsigned long long)s.on_screen);
    if (s.on_screen && r->surfaces[@(s.on_screen)]) [r present:s.on_screen];
    else [r drawCursor];
    snap_free(&s);
    hv_pvg_bytes_release(&bytes); /* the parts and records pointed into them */
}

/* Before the CPUs run: done when this returns, and the GPU's part queued ahead of anything the guest
   gives it. Takes bytes and releases them. Not on the worker. */
void hv_pvg_metal_restore(HvPvgMetal *m, HvPvgBytes bytes) {
    if (!m) { hv_pvg_bytes_release(&bytes); return; }
    HvReplay *r = (__bridge HvReplay *)m;
    dispatch_sync(r->worker, ^{
        @autoreleasepool {
            snap_restore_on_worker(r, bytes);
        }
    });
}

/* For tests: a texture's first level and slice, read or written. 0, or -1. */
int hv_pvg_metal_test_texture(HvPvgMetal *m, uint32_t task, uint32_t ref, void *bytes, uint32_t row, int write) {
    if (!m) return -1;
    HvReplay *r = (__bridge HvReplay *)m;
    __block int rc = -1;
    dispatch_sync(r->worker, ^{
        @autoreleasepool {
            HvObject *o = [r object:ref task:task];
            id<MTLTexture> t = o ? o->object : nil;
            if (![t conformsToProtocol:@protocol(MTLTexture)]) return;
            NSUInteger len = (NSUInteger)row * t.height;
            id<MTLBuffer> b = [r->device newBufferWithLength:len options:MTLResourceStorageModeShared];
            if (write) memcpy(b.contents, bytes, len);
            id<MTLCommandBuffer> cb = [r->queue commandBuffer];
            id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            if (write)
                [e copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:len sourceSize:MTLSizeMake(t.width, t.height, 1)
                        toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            else
                [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1)
                          toBuffer:b destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:len];
            [e endEncoding];
            [cb commit];
            if (!cb_wait(r, cb, HV_PVG_SNAP_GPU_WAIT, NULL) || cb.status != MTLCommandBufferStatusCompleted) return;
            if (!write) memcpy(bytes, b.contents, len);
            rc = 0;
        }
    });
    return rc;
}

/* For tests: a buffer's or a heap's bytes on the GPU, from its start, read or written. 0, or -1. */
int hv_pvg_metal_test_bytes(HvPvgMetal *m, uint32_t task, uint32_t ref, void *bytes, uint32_t len, int write) {
    if (!m || !len) return -1;
    HvReplay *r = (__bridge HvReplay *)m;
    __block int rc = -1;
    dispatch_sync(r->worker, ^{
        @autoreleasepool {
            HvObject *o = [r object:ref task:task];
            id<MTLBuffer> to = o ? (o->heap_whole ? o->heap_whole : o->object) : nil;
            if (![to conformsToProtocol:@protocol(MTLBuffer)] || to.length < len) return;
            id<MTLBuffer> b = [r->device newBufferWithLength:len options:MTLResourceStorageModeShared];
            if (write) memcpy(b.contents, bytes, len);
            id<MTLCommandBuffer> cb = [r->queue commandBuffer];
            id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
            if (write) [e copyFromBuffer:b sourceOffset:0 toBuffer:to destinationOffset:0 size:len];
            else [e copyFromBuffer:to sourceOffset:0 toBuffer:b destinationOffset:0 size:len];
            [e endEncoding];
            [cb commit];
            if (!cb_wait(r, cb, HV_PVG_SNAP_GPU_WAIT, NULL) || cb.status != MTLCommandBufferStatusCompleted) return;
            if (!write) memcpy(bytes, b.contents, len);
            rc = 0;
        }
    });
    return rc;
}

/* For tests: an info question (tags 450...), as an exec asks it. */
void hv_pvg_metal_test_info(HvPvgMetal *m, uint32_t task, uint32_t tag, const void *rec, uint32_t size) {
    if (!m) return;
    HvReplay *r = (__bridge HvReplay *)m;
    NSData *copy = [NSData dataWithBytes:rec length:size];
    dispatch_sync(r->worker, ^{
        @autoreleasepool {
            [r info:tag rec:(Rec){copy.bytes, (uint32_t)copy.length} task:task];
        }
    });
}

#ifdef HV_PVG_TEST_HOOKS /* the tests' build only (the Makefile's pvg_metal_test.o) */
/* For tests: a command buffer's failure with this code, as its completion handler would give it */
void hv_pvg_metal_test_gpu_failed(HvPvgMetal *m, long code) {
    if (!m) return;
    HvReplay *r = (__bridge HvReplay *)m;
    dispatch_sync(r->worker, ^{
        [r gpuFailed:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:code
                                     userInfo:@{NSLocalizedDescriptionKey: @"a failure made up by a test"}]];
    });
}

/* For tests: the indirect draws drawn from clamped arguments so far; the last clamped pass's jobs (HvClampJob, at most
   max) into jobs, and how many it had into n */
uint64_t hv_pvg_metal_test_indirect(HvPvgMetal *m, void *jobs, uint32_t max, uint32_t *n) {
    if (!m) return 0;
    HvReplay *r = (__bridge HvReplay *)m;
    __block uint64_t drawn = 0;
    dispatch_sync(r->worker, ^{
        drawn = r->indirect_drawn;
        NSData *last = r->clamp_jobs_last;
        uint32_t have = (uint32_t)(last.length / sizeof(HvClampJob));
        *n = have;
        if (last) memcpy(jobs, last.bytes, (have < max ? have : max) * sizeof(HvClampJob));
    });
    return drawn;
}

/* For tests: simple objects at refs first..first+7 of a task, as the guest's would be (its table's entries there
   left as they are): a render pipeline drawing BGRA8 colour 0, one of triangle patches of 3 points, a tile pipeline,
   a compute pipeline, a depth-stencil state, a sampler, a fence and a 64 KB heap. The number made, or -1. */
int hv_pvg_metal_test_objects(HvPvgMetal *m, uint32_t task, uint32_t first) {
    if (!m) return -1;
    HvReplay *r = (__bridge HvReplay *)m;
    __block int made = -1;
    dispatch_sync(r->worker, ^{
        @autoreleasepool {
            id<MTLDevice> dev = r->device;
            NSError *err = nil;
            id<MTLLibrary> lib = [dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
                "vertex float4 v(uint i [[vertex_id]]) { return float4(0, 0, 0, 1); }\n"
                "fragment half4 f() { return half4(1); }\n"
                "[[patch(triangle, 3)]] vertex float4 pv(float3 b [[position_in_patch]]) { return float4(b, 1); }\n"
                "kernel void k(uint3 i [[thread_position_in_grid]]) {}\n"
                "kernel void t(ushort2 i [[thread_position_in_threadgroup]]) {}\n" options:nil error:&err];
            if (!lib) { fprintf(stderr, "test objects: %s\n", err.localizedDescription.UTF8String); return; }
            MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = [lib newFunctionWithName:@"v"], d.fragmentFunction = [lib newFunctionWithName:@"f"];
            d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            id render = [dev newRenderPipelineStateWithDescriptor:d error:&err];
            HvTargets plain = pipeline_targets(d, NO);
            d.vertexFunction = [lib newFunctionWithName:@"pv"];
            d.maxTessellationFactor = 16;
            d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatchAndPerInstance; /* factors read both ways */
            id patches = [dev newRenderPipelineStateWithDescriptor:d error:&err];
            MTLTileRenderPipelineDescriptor *t = [MTLTileRenderPipelineDescriptor new];
            t.tileFunction = [lib newFunctionWithName:@"t"];
            t.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            id tile = [dev newRenderPipelineStateWithTileDescriptor:t options:0 reflection:nil error:&err];
            HvTargets tiles = pipeline_targets(t, YES);
            id compute = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&err];
            id depth = [dev newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
            id sampler = [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
            id fence = [dev newFence];
            MTLHeapDescriptor *h = [MTLHeapDescriptor new];
            h.size = 65536, h.storageMode = MTLStorageModePrivate;
            id heap = [dev newHeapWithDescriptor:h];
            id objects[8] = {render, patches, tile, compute, depth, sampler, fence, heap};
            HvKind kinds[8] = {KIND_RENDER_PIPELINE, KIND_RENDER_PIPELINE, KIND_TILE_PIPELINE, KIND_COMPUTE_PIPELINE,
                               KIND_DEPTH_STENCIL, KIND_SAMPLER, KIND_FENCE, KIND_HEAP};
            uint64_t list = hv_pvg_lite_task_objects(r->lite, task);
            HvTaskObjects *objs = task_objects(r, task, YES);
            made = 0;
            for (uint32_t i = 0; i < 8; i++) {
                HvObject *o = [HvObject new];
                if (!objects[i] || !list || hv_pvg_lite_task_read(r->lite, task, list + (uint64_t)(first + i) * 12, o->entry, 12)) continue;
                o->object = objects[i], o->kind = kinds[i];
                o->patch_points = i == 1 ? 3 : -1, o->tess_weight = i == 1 ? 16 * 16 : 1, o->patch_rec = i == 1 ? 8 : 0;
                o->patch_step = i == 1 ? 3 : 0;
                o->targets = i == 2 ? tiles : plain;
                if (!objs || !objs_set(objs, first + i, o)) continue;
                made++;
            }
        }
    });
    return made;
}
#endif
