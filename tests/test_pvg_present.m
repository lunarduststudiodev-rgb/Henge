/* pvg_metal.m's exec, commit and presenter scheduling, with a command queue whose buffers complete when the test
   says: no GPU and no timing. macOS draws into the surface it shows with no swap for some changes, so the screen
   is shown again after GPU work that writes it, and only then (an idle screen isn't drawn over and over). Also: the
   mapper's fence completes after the work before it, in order, without waiting, with the mapper's generation it
   was asked in; a video surface is written only once the GPU has finished reading it, and only waits for that when
   a row changed; what an exec draws into a video surface is not overwritten by the guest's older bytes, and goes
   back to the guest when it asks (sync_after): only the rows the GPU changed, the guest's own writing meanwhile kept,
   and once all of it is back the guest's writing goes up again. The GPU fault breaker: work that fails on the GPU in
   a way only the guest causes (a timeout, a page fault, a stack overflow) stops the guest's GPU work until the app
   re-arms it, its execs still finishing; other failures only 3 within 10 s, NotPermitted never. And the cap on the
   guest's command buffers in flight (HV_PVG_GUEST_INFLIGHT): the next commit waits for a slot, on the worker only. */
/* The GPU mappings' registry (core/ramfile.h) is the test's: every map is unmapped once, and only a map */
#define hv_ramfile_gpu_map test_gpu_map
#define hv_ramfile_gpu_unmap test_gpu_unmap
#define hv_ramfile_gpu_relist test_gpu_relist
/* And its vm_remaps, counted (test_map_runs) */
#include <mach/mach.h>
static unsigned vm_remaps, vm_remap_fails; /* the next vm_remap_fails of them fail (test_map_pin_first) */
/* Then no page list can be had (core/ramfile.c without the memory for one): a map adds none, a relist keeps the old */
static int vm_remap_fail_lists, gpu_list_fails;
static kern_return_t test_vm_remap(vm_map_t t, vm_address_t *to, vm_size_t n, vm_address_t mask, int flags, vm_map_t from_task,
                                   vm_address_t from, boolean_t copy, vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inherit) {
    vm_remaps++;
    if (vm_remap_fails) {
        vm_remap_fails--;
        if (vm_remap_fail_lists) gpu_list_fails = 1;
        return KERN_NO_SPACE;
    }
    return vm_remap(t, to, n, mask, flags, from_task, from, copy, cur, max, inherit);
}
#define HV_VM_REMAP test_vm_remap
static int seg_data_fails; /* an exec's segments not allocated (test_exec_no_memory) */
#define HV_PVG_SEG_DATA(n) (seg_data_fails ? nil : [[NSMutableData alloc] initWithLength:(n)])
#define HV_PVG_EXEC_INDIRECT 3 /* an exec's indirect draws that are clamped (test_clamp_exec_cap) */
#define HV_PVG_HOLD_MAX (300 * NSEC_PER_MSEC) /* test_hold: the longest hold, short */
/* test_gpu_never_done: the longest waits, short; long enough over its "at once" (< 0.5 s) that a busy Mac running the
   tests doesn't make one look like the other */
#define HV_PVG_GPU_WAIT (1 * NSEC_PER_SEC)
#define HV_PVG_SNAP_GPU_WAIT (1 * NSEC_PER_SEC)
#ifdef HV_PVG_TEST_NO_GPU /* test_pvg_present_nogpu: built so no GPU can be reached at all (make test runs it on Macs whose
                             GPU is left alone); the two clamp tests, the only ones that want one, say they are skipped */
#import <Metal/Metal.h>
#define MTLCreateSystemDefaultDevice() ((id<MTLDevice>)nil)
#endif
#include <stdlib.h>
static unsigned getenv_calls; /* test_env_once: every read of the environment pvg_metal.m makes */
static char *counted_getenv(const char *name);
#define getenv counted_getenv
#include "../devices/pvg_metal.m"
#undef getenv
static char *counted_getenv(const char *name) { getenv_calls++; return getenv(name); }
#pragma clang diagnostic ignored "-Wunused-function" /* each of the two builds runs only its own tests */

static const void *gpu_mapped[64];
static unsigned ngpu_mapped, gpu_maps, gpu_unmaps, gpu_bad_unmaps, gpu_relists, gpu_relist_listed;
/* Each map's vm_remaps made before it, and its pages listed (test_map_pin_first) */
static unsigned gpu_map_remaps[64], gpu_map_listed[64];
void test_gpu_map(const void *m, void *const *src, size_t n) {
    unsigned listed = 0;
    for (size_t i = 0; src && i < n; i++) listed += src[i] != NULL;
    gpu_map_remaps[gpu_maps % 64] = vm_remaps, gpu_map_listed[gpu_maps % 64] = listed;
    gpu_maps++;
    if (ngpu_mapped < 64 && !gpu_list_fails) gpu_mapped[ngpu_mapped++] = m;
}
/* As core/ramfile.c: in the order made, an unmap taking the oldest of its key */
void test_gpu_unmap(const void *m) {
    gpu_unmaps++;
    for (unsigned i = 0; i < ngpu_mapped; i++)
        if (gpu_mapped[i] == m) {
            memmove(&gpu_mapped[i], &gpu_mapped[i + 1], (--ngpu_mapped - i) * sizeof *gpu_mapped);
            return;
        }
    gpu_bad_unmaps++;
}
void test_gpu_relist(const void *m, void *const *src, size_t n) {
    (void)m;
    gpu_relists++, gpu_relist_listed = 0;
    for (size_t i = 0; i < n; i++) gpu_relist_listed += src[i] != NULL;
}

/* A surface's guest memory (test_video_shared): its page list in the first page, its pixels in the second; the
   descriptors of mappings 5 and 6 (the same memory and layout) and 7 (the same pages from another offset); and the
   record that makes a texture over one of them, at 0x8000 in task 1. */
static uint8_t *guest_ram;
static uint8_t surface_desc[2][0x400];
static uint8_t surface_record[128];
static uint32_t sampler_record[7]; /* task 1 at 0x9000: a serialized sampler (test_sampler_made) */
static uint8_t tex12_record[256];  /* task 1 at 0x9800: a type-12 texture entry (test_texture_type12) */

/* Guest memory: task 1's object table, {type | size << 8, at, at >> 32} per ref; nothing else is read */
static uint32_t table[16][3];
/* Task 2: VIEWS refs, each a view (tag 7) of the ref views_parent[ref] names; objects at 0x100000, records 0x40 apart
   from 0x200000. Ref 1 is an empty slot. */
#define VIEWS 20000
static uint32_t views_parent[VIEWS];
static unsigned task_reads; /* test_lookup_once */
static uint32_t task3_table[8][3]; /* task 3's refs 2 to 7 (test_info_null) */
static uint32_t gamma_table[3 * 257]; /* task 0's at 0x7000, where the colour probe looks (test_colour_probe) */
int hv_pvg_lite_task_read(HvPvgLite *p, uint32_t t, uint64_t a, void *o, uint32_t n) {
    (void)p;
    if (a == 0x7000) return t == 0 && n <= sizeof gamma_table ? (memcpy(o, gamma_table, n), 0) : -1;
    task_reads++;
    if (t == 2 && n == 12 && a >= 0x100000 && (a - 0x100000) % 12 == 0 && (a - 0x100000) / 12 < VIEWS) {
        uint32_t ref = (uint32_t)((a - 0x100000) / 12), e[3] = {ref == 1 ? 0 | 12 << 8 : 8 | 0x40 << 8, 0x200000 + ref * 0x40, 0};
        return memcpy(o, e, 12), 0;
    }
    if (t == 2 && a >= 0x200000 && (a - 0x200000) % 0x40 == 0 && (a - 0x200000) / 0x40 < VIEWS && n <= 0x40) {
        uint32_t ref = (uint32_t)((a - 0x200000) / 0x40), rec[16] = {7, 0x40, ref, views_parent[ref], 80 /* BGRA8 */};
        return memcpy(o, rec, n), 0;
    }
    if (t == 1 && a == 0x8000 && n <= sizeof surface_record) return memcpy(o, surface_record, n), 0;
    if (t == 1 && a == 0x9000 && n <= sizeof sampler_record) return memcpy(o, sampler_record, n), 0;
    if (t == 1 && a == 0x9800 && n <= sizeof tex12_record) return memcpy(o, tex12_record, n), 0;
    if (t == 3 && a == 0x3000 + 12 && n == 12) { /* task 3's ref 1: a buffer (test_tile_size_pages) */
        uint32_t e[3] = {1 | 16 << 8, 0x3100, 0};
        return memcpy(o, e, 12), 0;
    }
    if (t == 3 && n == 12 && a >= 0x3000 + 24 && a < 0x3000 + 96 && (a - 0x3000) % 12 == 0) return memcpy(o, task3_table[(a - 0x3000) / 12], 12), 0;
    if (t != 1 || n != 12 || a < 0x1000 || (a - 0x1000) % 12 || (a - 0x1000) / 12 >= 16) abort();
    memcpy(o, table[(a - 0x1000) / 12], 12);
    return 0;
}
/* What reached the device's worker copy of the task tables (test_task_commands): op, length, first word, on the worker */
static uint32_t told_task[4][4], ntold_task;
void hv_pvg_lite_task_command(HvPvgLite *p, uint32_t op, const uint32_t *a, uint32_t n) {
    (void)p;
    if (ntold_task < 4)
        told_task[ntold_task][0] = op, told_task[ntold_task][1] = n, told_task[ntold_task][2] = n ? a[0] : ~0u,
        told_task[ntold_task][3] = !strcmp(dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL), "test.worker");
    ntold_task++;
}
uint64_t hv_pvg_lite_task_objects(HvPvgLite *p, uint32_t t) { (void)p; return t == 1 ? 0x1000 : t == 2 ? 0x100000 : t == 3 ? 0x3000 : 0; }
/* Task 3's page table (test_map_runs): guest RAM's pages 0x100 on are task3_ram's 32 */
static uint32_t task3_pages[32];
static uint8_t *task3_ram;
void hv_pvg_lite_task_pages(HvPvgLite *p, uint32_t t, uint64_t a, uint64_t n, uint32_t *o) {
    (void)p;
    if (t != 3 || a + n > 32) abort();
    memcpy(o, task3_pages + a, n * 4);
}
uint8_t *hv_pvg_lite_page(HvPvgLite *p, uint32_t n) {
    (void)p;
    if (!task3_ram) abort();
    return n >= 0x100 && n < 0x120 ? task3_ram + ((size_t)(n - 0x100) << 14) : NULL; /* else: outside RAM */
}
void *hv_pvg_lite_phys(HvPvgLite *p, uint64_t a, uint64_t n) {
    (void)p;
    if ((a == 0x10000 || a == 0x20000) && n <= 0x4000 && guest_ram) return guest_ram + (a == 0x10000 ? 0 : 0x4000);
    if (a == 0x22000 && n <= 0x4000 && guest_ram) return guest_ram + 0x6000; /* a page not on a 16 KB boundary (test_surface_misaligned) */
    if (a == 0x30000 && n <= 0x4000 && guest_ram) return guest_ram + 0x8000; /* a second page list (test_video_key) */
    abort();
}
const uint8_t *hv_pvg_lite_surface(HvPvgLite *p, uint64_t n) {
    (void)p;
    if (n == 5 || n == 6) return surface_desc[0];
    if (n == 7) return surface_desc[1];
    abort();
}
static const uint8_t *test_display_page; /* test_cursor_scaled */
const uint8_t *hv_pvg_lite_display_page(HvPvgLite *p) { (void)p; return test_display_page; }
static unsigned swaps_told; /* op 8s that reached the display (test_swap_routed): not taken, so nothing to present */
int hv_pvg_lite_command(HvPvgLite *p, const uint32_t *c, uint32_t n) { /* test_task_commands' object list, and a swap */
    (void)p, (void)n;
    if ((c[0] & 0xffff) == 8) return swaps_told++, 0;
    if ((c[0] & 0xffff) != 0x44) abort();
    return 1;
}
static uint32_t test_pointer_w, test_pointer_h; /* the display mode a scaled picture is not (test_cursor_scaled); 0: the same */
void hv_pvg_lite_pointer_space(uint32_t sw, uint32_t sh, uint32_t *w, uint32_t *h) {
    *w = test_pointer_w ? test_pointer_w : sw, *h = test_pointer_h ? test_pointer_h : sh;
}
int hv_pvg_lite_retina(void) { return 0; }
int hv_pvg_tracing(void) { return 0; }
/* Saving and restoring aren't tested here (test_pvg_snap is) */
void hv_pvg_blob_put(HvPvgBlob *b, const void *v, size_t n) { (void)b, (void)v, (void)n; abort(); }
void hv_pvg_blob_get(HvPvgReader *rd, void *v, size_t n) { (void)rd, (void)v, (void)n; abort(); }
void hv_pvg_bytes_release(HvPvgBytes *b) { (void)b; abort(); }
static unsigned stamps;
static void (^on_stamp)(uint32_t stamp); /* what the guest would see as it gets the stamp */
void hv_pvg_lite_complete(HvPvgLite *p, uint32_t c, uint32_t s) {
    (void)p, (void)c;
    stamps++;
    if (on_stamp) on_stamp(s);
}
static uint64_t mapper_done[8], mapper_gen[8];
static unsigned nmapper_done;
void hv_pvg_lite_mapper_done(HvPvgLite *p, uint64_t index, uint64_t gen) {
    (void)p;
    if (nmapper_done < 8) mapper_gen[nmapper_done] = gen, mapper_done[nmapper_done++] = index;
}

static unsigned waits; /* waitUntilCompleted on a buffer the GPU hadn't finished */
static unsigned hung_waits; /* waitUntilCompleted on one that never finishes (a real wait: for ever; here 1 s) */
static dispatch_queue_t test_complete_q; /* the replay's complete_q: a buffer's finish waits for what its handlers put there */
@interface TestCB : NSObject
@property(nonatomic, strong) NSMutableArray *handlers;
@property(nonatomic, strong) NSError *error;
@property MTLCommandBufferStatus status;
@property CFTimeInterval GPUStartTime, GPUEndTime; /* read by the completion handler (late_probe) */
@property BOOL hung; /* the GPU never finishes it (test_gpu_never_done) */
@property(nonatomic, strong) id presented; /* the drawable it presents */
@end
@implementation TestCB
- (instancetype)init { if ((self = [super init])) _handlers = [NSMutableArray new], _status = MTLCommandBufferStatusNotEnqueued; return self; }
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler { [_handlers addObject:[handler copy]]; }
- (void)enqueue { _status = MTLCommandBufferStatusEnqueued; }
- (void)commit { _status = MTLCommandBufferStatusCommitted; }
- (void)presentDrawable:(id)drawable { _presented = drawable; } /* presenter_commit */
- (void)finish:(BOOL)success {
    [self finishWithError:success ? nil : [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorTimeout userInfo:nil]];
}
- (void)finishWithError:(NSError *)error {
    _status = error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
    _error = error;
    NSArray *callbacks = [_handlers copy];
    [_handlers removeAllObjects];
    for (MTLCommandBufferHandler callback in callbacks) callback((id<MTLCommandBuffer>)self);
    if (test_complete_q) dispatch_sync(test_complete_q, ^{}); /* the guest's side (copy-backs, stamps) done too */
}
- (void)waitUntilCompleted {
    if (_hung) { hung_waits++, usleep(1000 * 1000); return; } /* stands for for ever, and is counted */
    if (_status < MTLCommandBufferStatusCompleted) waits++, [self finish:YES]; /* the GPU finishing meanwhile */
}
@end
#ifdef HV_PVG_TEST_HOOKS
/* The worker's bounded waits (cb_wait) on these: as waitUntilCompleted, a buffer finishing meanwhile. test_gpu_never_done
   puts back the real one, which goes by the status. */
static BOOL fake_cb_wait(id<MTLCommandBuffer> cb, uint64_t ns) {
    (void)ns;
    [cb waitUntilCompleted];
    return cb.status >= MTLCommandBufferStatusCompleted;
}
#endif

/* Hands out new buffers, keeping each in order */
static os_unfair_lock *made_lock; /* the replay's screen_lock: [queue commandBuffer] waits while the queue is full, so never under it */
static unsigned made_locked;
@interface TestQueue : NSObject
@property(nonatomic, strong) NSMutableArray<TestCB *> *made;
@end
@implementation TestQueue
- (instancetype)init { if ((self = [super init])) _made = [NSMutableArray new]; return self; }
- (id)commandBuffer {
    if (made_lock) { /* the presenter's thread holds it only for a moment; this one's, until this returns */
        int tries = 0;
        while (!os_unfair_lock_trylock(made_lock) && ++tries < 1000) usleep(10);
        if (tries < 1000) os_unfair_lock_unlock(made_lock);
        else made_locked++;
    }
    TestCB *cb = [TestCB new];
    [_made addObject:cb];
    return cb;
}
@end

@interface TestReplay : HvReplay {
@public
    unsigned displayed;
}
@end
@implementation TestReplay
- (void)presentLoop { /* the real dirty flag, lock and kickPresenter; no drawing */
    os_unfair_lock_lock(&screen_lock);
    if (screen_dirty) displayed++;
    screen_dirty = NO;
    presenter_running = NO;
    os_unfair_lock_unlock(&screen_lock);
}
@end

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static TestReplay *r;
static TestQueue *q;
/* The Metal side's copy of the mapper's state: an id held unmapped, and none held */
static BOOL is_unmapped(uint32_t id) { return r->unmapped[id / 8] >> (id % 8) & 1; }
static BOOL no_unmapped(void) {
    for (size_t i = 0; i < sizeof r->unmapped; i++)
        if (r->unmapped[i]) return NO;
    return YES;
}
static HvObject *object(uint32_t ref) {
    table[ref][0] = 0x77 | 12 << 8, table[ref][1] = ref; /* already made: the test sets it up */
    HvObject *o = [HvObject new];
    memcpy(o->entry, table[ref], 12);
    task_objects(r, 1, YES)[@(ref)] = o;
    return o;
}
/* An exec using one ref with these flags (bit 1: the exec writes it); its buffer, not finished */
static TestCB *exec_ref(uint32_t ref, uint32_t flags, uint32_t stamp) {
    uint32_t a[5] = {1, 1, 0, ref, flags};
    BOOL ok = [r exec:a length:sizeof a channel:1 stamp:stamp];
    CHECK(ok, "exec taken");
    return q.made.lastObject;
}
static unsigned shown(void) { dispatch_sync(r->presenter, ^{}); return r->displayed; }

static void test_redraw(void) {
    printf("redraw after GPU work that writes the screen\n");
    id<MTLTexture> screen = (id<MTLTexture>)[NSObject new], other = (id<MTLTexture>)[NSObject new];
    r->screen_tex = screen;
    HvObject *on = object(5), *off = object(6), *view = object(7), *surface_tex = object(8);
    on->scanout = screen, off->scanout = other;
    view->parent = on; /* made from the surface on screen */
    surface_tex->surface = [HvObject new], surface_tex->surface->scanout = screen;

    TestCB *cb = exec_ref(5, 2, 1);
    CHECK(shown() == 0, "not before the GPU is done");
    [cb finish:YES];
    CHECK(shown() == 1 && stamps == 1, "once it is: shown %u, stamps %u", r->displayed, stamps);
    for (uint32_t i = 0; i < 3; i++) { /* every time, the screen idle in between */
        cb = exec_ref(5, 2, 2 + i);
        [cb finish:YES];
        CHECK(shown() == 2 + i, "again after idle: shown %u", r->displayed);
    }
    unsigned before = shown();
    [exec_ref(5, 1, 10) finish:YES];
    CHECK(shown() == before, "reading the screen's surface: not shown again");
    [exec_ref(6, 2, 11) finish:YES];
    CHECK(shown() == before, "writing another surface: not shown again");
    [exec_ref(7, 2, 12) finish:YES];
    CHECK(shown() == before + 1, "writing a view of the screen's surface: shown");
    [exec_ref(8, 2, 13) finish:YES];
    CHECK(shown() == before + 2, "writing a texture of the screen's surface: shown");
    unsigned failed_before = r->failed, stamps_before = stamps;
    [exec_ref(5, 2, 14) finish:NO];
    CHECK(shown() == before + 2 && r->failed == failed_before + 1 && stamps == stamps_before + 1, "failed work: its stamp, no redraw");
    atomic_store(&r->gpu_faulted, 0); /* the breaker's own test is test_breaker */
    /* a stamp waiting behind GPU work: no redraw of its own */
    cb = exec_ref(6, 2, 15);
    [r finishChannel:1 stamp:16];
    [cb finish:YES], [q.made.lastObject finish:YES];
    CHECK(shown() == before + 2 && stamps == stamps_before + 3, "a stamp fence: no redraw");
    CHECK(waits == 0 && atomic_load(&r->inflight) == 0 && !r->screen_dirty && !r->presenter_running, "never waited, all done, idle");
}

/* gfx-render 1: a swap's stamp says the buffer swapped away from is free to draw into. The presenter takes the screen's
   texture well before it commits the frame that reads it; a stamp given back in between (as finishChannel's, which
   joins a trail from before the frame, or comes at once with no work in flight) lets the guest draw its next frame
   into the buffer before or while the frame reads it. Here an earlier stamp's trail is still to run when the
   presenter takes the screen, its frame still being encoded; then the swap. */
static void settle(void);
static void test_swap_waits_for_presenter(void) {
    printf("a swap's stamp after the presenter's frame that read the old buffer\n");
    unsigned s0 = stamps;
    TestCB *work = exec_ref(6, 2, 40);
    dispatch_sync(r->worker, ^{ [r finishChannel:1 stamp:41]; }); /* a trail behind the guest's work */
    TestCB *trail = q.made.lastObject;
    CHECK(trail != work, "the earlier stamp: a trail of its own");
    TestCB *frame = [TestCB new];
    os_unfair_lock_lock(&r->screen_lock);
    presenter_take(r, (id<MTLCommandBuffer>)frame); /* the screen's texture taken; the frame still being encoded */
    os_unfair_lock_unlock(&r->screen_lock);
    CHECK(frame.status == MTLCommandBufferStatusEnqueued, "its place on the GPU taken with the texture");
    NSUInteger made = q.made.count;
    dispatch_sync(r->worker, ^{ [r finishSwap:1 stamp:42]; });
    CHECK(q.made.count == made + 1, "the swap's stamp: not with the trail from before the frame, a buffer after it");
    TestCB *after = q.made.lastObject;
    [work finish:YES], [trail finish:YES];
    settle();
    CHECK(stamps == s0 + 2, "the work's and the earlier stamp, not yet the swap's (%u)", stamps - s0);
    [frame commit], [frame finish:YES], [after finish:YES];
    settle();
    CHECK(stamps == s0 + 3 && atomic_load(&r->inflight) == 0, "the swap's once the frame is done (%u), nothing in flight", stamps - s0);
    /* Nothing else in flight: the frame alone holds the stamp back */
    frame = [TestCB new];
    os_unfair_lock_lock(&r->screen_lock);
    presenter_take(r, (id<MTLCommandBuffer>)frame);
    os_unfair_lock_unlock(&r->screen_lock);
    dispatch_sync(r->worker, ^{ [r finishSwap:1 stamp:43]; });
    CHECK(stamps == s0 + 3, "not at once while the frame is in flight");
    after = q.made.lastObject;
    [frame commit], [frame finish:YES], [after finish:YES];
    settle();
    CHECK(stamps == s0 + 4 && atomic_load(&r->inflight) == 0 && waits == 0, "then, without a wait");
}

/* gfx-cost 2 and gfx-commands F3: a buffer over the guest's pages is one vm_remap for each run of pages consecutive in
   memory, not one a page; and a page number outside RAM in its table (mapped as zeros) is the guest's number still, so
   the next exec's look at the table (refresh_pages) finds it unchanged instead of remapping it every exec. */
static void test_map_runs(void) {
    printf("a mapping: a remap for each run of pages; one outside RAM not remapped every exec\n");
    vm_address_t ram = 0;
    if (vm_allocate(mach_task_self(), &ram, 32 << 14, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { CHECK(0, "no guest memory"); return; }
    task3_ram = (uint8_t *)ram;
    for (int i = 0; i < 32; i++) task3_ram[(size_t)i << 14] = (uint8_t)(i + 1);
    for (int i = 0; i < 16; i++) task3_pages[i] = 0x100 + i;      /* a run */
    task3_pages[16] = 0x9999;                                      /* outside RAM */
    for (int i = 17; i < 24; i++) task3_pages[i] = 0x118 + i - 17; /* another run */
    task3_pages[24] = 0;                                           /* none */
    HvObject *o = [HvObject new];
    o->length = 25 << 14;
    r->max_buffer = 1ull << 30;
    unsigned remaps_before = vm_remaps, maps0 = gpu_maps;
    CHECK(map_pages(r, 3, o), "mapped");
    CHECK(vm_remaps == remaps_before + 2 && gpu_maps == maps0 + 1, "two runs: two remaps (%u)", vm_remaps - remaps_before);
    BOOL same = YES;
    for (int i = 0; i < 16; i++) same = same && o->mapped[(size_t)i << 14] == i + 1;
    for (int i = 17; i < 24; i++) same = same && o->mapped[(size_t)i << 14] == 0x18 + i - 17 + 1;
    CHECK(same && o->mapped[(size_t)16 << 14] == 0 && o->mapped[(size_t)24 << 14] == 0, "the guest's bytes; zeros where none is");
    CHECK(o->pages[16] == 0x9999, "the guest's own number kept");
    CHECK(has_guest_pages(o), "some of it in RAM: it has guest pages");
    /* (review 6) all of its pages outside RAM: none to copy, not zeros over the GPU's copy */
    HvObject *outside = [HvObject new];
    outside->length = 1 << 14, outside->vpage = 16;
    CHECK(map_pages(r, 3, outside) && outside->pages[0] == 0x9999, "one wholly outside RAM: mapped, the number kept");
    CHECK(!has_guest_pages(outside), "and it has no guest pages");
    test_gpu_unmap(outside->mapped);
    vm_deallocate(mach_task_self(), (vm_address_t)outside->mapped, 1 << 14);
    free(outside->pages), outside->pages = NULL, outside->mapped = NULL;
    uint8_t *m = o->mapped;
    uint64_t remaps0 = r->remaps;
    refresh_pages(r, 3, o);
    CHECK(o->mapped == m && r->remaps == remaps0 && vm_remaps == remaps_before + 2, "the table unchanged: not remapped (%llu)",
          r->remaps - remaps0);
    task3_pages[3] = 0x11f; /* the guest moved a page: a run of 3, one page, a run of 12 */
    refresh_pages(r, 3, o);
    CHECK(r->remaps == remaps0 + 1 && o->mapped[(size_t)3 << 14] == 32 && vm_remaps == remaps_before + 2 + 4, "a page changed: remapped (%u)",
          vm_remaps - remaps_before - 2);
    test_gpu_unmap(o->mapped);
    vm_deallocate(mach_task_self(), (vm_address_t)o->mapped, 25 << 14);
    free(o->pages), o->pages = NULL, o->mapped = NULL;
    vm_deallocate(mach_task_self(), ram, 32 << 14);
    task3_ram = NULL;
    CHECK(gpu_bad_unmaps == 0, "unmapped once");
}

/* gfx-commands F1: the pointer drawn into the CPU frame under a scaled resolution (a picture bigger, or smaller, than
   the display mode the pointer's position is in) is the whole glyph stretched, and never reads past it. */
static void test_cursor_scaled(void) {
    printf("the pointer on a scaled picture: the whole glyph, nothing past it\n");
    const uint32_t W = 256, H = 144, G = 8;
    uint8_t *frame = calloc((size_t)W * H, 4), *base = calloc((size_t)W * H, 4);
    /* the glyph, then what is past it in memory: opaque, a colour the glyph has none of */
    uint8_t *glyph = malloc((size_t)G * G * 4 * 8);
    memset(glyph, 0xee, (size_t)G * G * 4 * 8);
    for (uint32_t i = 0; i < G * G; i++) glyph[i * 4] = 0x10, glyph[i * 4 + 1] = (uint8_t)(i / G), glyph[i * 4 + 2] = (uint8_t)(i % G), glyph[i * 4 + 3] = 255;
    uint8_t page[0x4000] = {0};
    uint32_t where = 10 | 10u << 16;
    memcpy(page + 0xe00, &where, 4);
    page[0xe04] = 1;
    uint32_t spaces[2][2] = {{192, 108}, {288, 162}}; /* the picture bigger than the mode, then smaller */
    for (int k = 0; k < 2; k++) {
        os_unfair_lock_lock(&r->frame_lock);
        memset(frame, 0, (size_t)W * H * 4), memset(base, 0, (size_t)W * H * 4);
        r->frame = frame, r->base = base, r->frame_w = W, r->frame_h = H, r->drawn_w = r->drawn_h = 0;
        r->cursor = glyph, r->cursor_w = r->cursor_h = G, r->cursor_stride = G * 4, r->cursor_hot_x = r->cursor_hot_y = 0;
        r->cursor_shown = YES, r->pointer_hidden = NO;
        test_display_page = page, test_pointer_w = spaces[k][0], test_pointer_h = spaces[k][1];
        [r drawCursorLocked];
        BOOL past = NO;
        int row_max = 0, col_max = 0;
        for (size_t i = 0; i < (size_t)W * H; i++) {
            if (frame[i * 4] == 0xee) past = YES;
            if (frame[i * 4] == 0x10) row_max = MAX(row_max, frame[i * 4 + 1]), col_max = MAX(col_max, frame[i * 4 + 2]);
        }
        CHECK(!past, "%s: nothing from past the glyph", k ? "smaller" : "bigger");
        CHECK(row_max == G - 1 && col_max == G - 1, "%s: all of the glyph (to row %d, column %d)", k ? "smaller" : "bigger", row_max, col_max);
        r->frame = r->base = NULL, r->cursor = NULL, r->drawn_w = r->drawn_h = 0;
        os_unfair_lock_unlock(&r->frame_lock);
    }
    test_display_page = NULL, test_pointer_w = test_pointer_h = 0;
    free(frame), free(base), free(glyph);
}

/* A render encoder that only says its tile size (test_tile_size_pages) */
@interface TileEncoder : NSObject
@property(readonly) NSUInteger tileWidth, tileHeight;
@end
@implementation TileEncoder
- (NSUInteger)tileWidth { return 32; }
- (NSUInteger)tileHeight { return 16; }
@end

/* gfx-commands F5: the tile size (render 164) goes into the buffer's pages as the guest's table has them now, not the
   pages it had when it was mapped, which the guest may have given to other memory since */
static void test_tile_size_pages(void) {
    printf("the tile size: into the buffer's pages as they are now\n");
    vm_address_t ram = 0;
    if (vm_allocate(mach_task_self(), &ram, 32 << 14, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { CHECK(0, "no guest memory"); return; }
    task3_ram = (uint8_t *)ram;
    for (int i = 0; i < 2; i++) task3_pages[i] = 0x100 + i;
    r->max_buffer = 1ull << 30;
    __block BOOL ok = NO;
    dispatch_sync(r->worker, ^{
        HvObject *o = [HvObject new];
        uint32_t e[3] = {1 | 16 << 8, 0x3100, 0};
        memcpy(o->entry, e, 12);
        o->length = 2 << 14, o->kind = KIND_BUFFER, o->object = (id)[NSObject new]; /* a buffer, as far as 164 looks */
        o->cache_task = 3, o->cache_ref = 1;
        ok = map_pages(r, 3, o);
        task_objects(r, 3, YES);
        r->tasks[3][@1] = o;
    });
    CHECK(ok, "mapped");
    task3_pages[1] = 0x105; /* the guest moved the buffer's second page */
    static const uint32_t rec[3] = {1, 0x4000 + 8, 0}; /* {buffer ref, offset}: on the second page */
    dispatch_sync(r->worker, ^{ [r render:(id<MTLRenderCommandEncoder>)[TileEncoder new] tag:164 rec:(Rec){(const uint8_t *)rec, 12} task:3]; });
    uint16_t now[2], before[2];
    memcpy(now, task3_ram + (5 << 14) + 8, 4), memcpy(before, task3_ram + (1 << 14) + 8, 4);
    CHECK(now[0] == 32 && now[1] == 16, "in the page the guest has there now (%u x %u)", now[0], now[1]);
    CHECK(before[0] == 0 && before[1] == 0, "not in the one it had");
    /* (review 3) moved again, to a page that can't be mapped (a hole here): nothing written through the old mapping */
    memset(task3_ram + (5 << 14) + 8, 0, 4);
    vm_deallocate(mach_task_self(), ram + (6 << 14), 1 << 14);
    task3_pages[1] = 0x106;
    uint64_t failed = r->remaps_failed;
    dispatch_sync(r->worker, ^{ [r render:(id<MTLRenderCommandEncoder>)[TileEncoder new] tag:164 rec:(Rec){(const uint8_t *)rec, 12} task:3]; });
    memcpy(before, task3_ram + (5 << 14) + 8, 4);
    __block BOOL gone = NO;
    dispatch_sync(r->worker, ^{ gone = r->tasks[3][@1]->gone; });
    CHECK(before[0] == 0 && before[1] == 0, "its pages moved and not mapped again: nothing in the old pages (%u x %u)", before[0], before[1]);
    CHECK(gone && r->remaps_failed == failed + 1, "and the buffer gone, made anew on its next use");
    dispatch_sync(r->worker, ^{
        HvObject *o = r->tasks[3][@1];
        [r->tasks[3] removeObjectForKey:@1];
        if (o->mapped && !o->mapping_in_buffer) {
            test_gpu_unmap(o->mapped);
            vm_deallocate(mach_task_self(), (vm_address_t)o->mapped, o->length);
            o->mapped = NULL;
        }
    });
    memset(task3_pages, 0, sizeof task3_pages);
    vm_deallocate(mach_task_self(), ram, 32 << 14);
    task3_ram = NULL;
}

/* gfx-commands F7: guest values Metal stops the app on (its own words, from this OS's Metal.framework), each checked
   before it reaches Metal. Fakes stand in for the device, encoders, buffer and texture: what they were given is kept. */
@interface SamplerDevice : NSObject
@property(nonatomic, strong) MTLSamplerDescriptor *made;
@end
@implementation SamplerDevice
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d { _made = [d copy]; return [NSObject new]; }
@end
@interface FakeBuffer : NSObject
@property(readonly) NSUInteger length;
@end
@implementation FakeBuffer
- (NSUInteger)length { return 4096; }
@end
@interface TessEncoder : NSObject
@property(nonatomic, strong) id buffer;
@property(nonatomic) NSUInteger offset, stride, calls;
@end
@implementation TessEncoder
- (void)setTessellationFactorBuffer:(id)b offset:(NSUInteger)o instanceStride:(NSUInteger)s { _buffer = b, _offset = o, _stride = s, _calls++; }
@end
@interface FakeTexture : NSObject
@property(nonatomic) NSUInteger pixelFormat;
@end
@implementation FakeTexture
- (NSUInteger)textureType { return MTLTextureType2D; }
- (NSUInteger)width { return 64; }
- (NSUInteger)height { return 64; }
- (NSUInteger)depth { return 1; }
- (NSUInteger)mipmapLevelCount { return 4; }
- (NSUInteger)sampleCount { return 1; }
- (NSUInteger)arrayLength { return 1; }
- (NSUInteger)usage { return MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; }
- (NSUInteger)cpuCacheMode { return 0; }
- (NSUInteger)storageMode { return MTLStorageModePrivate; }
- (NSUInteger)hazardTrackingMode { return 0; }
- (BOOL)isFramebufferOnly { return NO; }
@end
@interface MipEncoder : NSObject
@property(nonatomic) NSUInteger made;
@end
@implementation MipEncoder
- (void)generateMipmapsForTexture:(id)t { _made++; }
@end

static HvPassAttach attachment(uint32_t format, uint32_t w, uint32_t samples, uint64_t texture) {
    HvPassAttach a;
    memset(&a, 0, sizeof a);
    a.present = 1, a.texture = a.root = texture;
    a.tex = (HvTexDesc){samples > 1 ? HV_TEX_2D_MS : HV_TEX_2D, format, w, w, 1, 1, samples, 1, MTLTextureUsageRenderTarget, 2 << 4};
    return a;
}

static void test_sampler_made(void) {
    printf("guest values Metal would stop the app on (F7): the sampler\n");
    /* A sampler in pixel coordinates with two filters: "non-normalized coords require minFilter==magFilter" */
    SamplerDevice *dev = [SamplerDevice new];
    r->device = (id)dev;
    uint64_t bits = 0 | 1 << 2; /* min nearest, mag linear, pixel coordinates (bit 31 clear) */
    float lod[2] = {0, 0};
    sampler_record[0] = 3, sampler_record[1] = 28, sampler_record[2] = 30;
    memcpy(&sampler_record[3], &bits, 8), memcpy(&sampler_record[5], lod, 8);
    __block HvKind kind = KIND_NONE;
    dispatch_sync(r->worker, ^{ [r makeSerialized:0x9000 length:28 task:1 kind:&kind into:[HvObject new]]; });
    CHECK(kind == KIND_SAMPLER && dev.made && !dev.made.normalizedCoordinates, "the sampler made");
    CHECK(dev.made.minFilter == dev.made.magFilter, "in pixel coordinates, one filter for both (%lu, %lu)",
          (unsigned long)dev.made.minFilter, (unsigned long)dev.made.magFilter);
    r->device = nil;
}

/* What pvg_metal.m relies on in a texture descriptor: the same checks against Metal's own (test_pvg_present_nogpu) and
   the stand-in test_pvg_present_nometal has instead (tests/pvg_nometal_stubs.m), so the stand-in can't differ */
static void test_texture_descriptor(void) {
    printf("a texture descriptor: defaults, the packed resource options, a copy\n");
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    CHECK(d.textureType == MTLTextureType2D && d.pixelFormat == MTLPixelFormatRGBA8Unorm && d.width == 1 && d.height == 1 &&
              d.depth == 1 && d.mipmapLevelCount == 1 && d.sampleCount == 1 && d.arrayLength == 1,
          "the defaults: 2D, RGBA8Unorm, 1 x 1 x 1, one level, one sample, one slice");
    CHECK(d.usage == MTLTextureUsageShaderRead && d.allowGPUOptimizedContents, "used for reading, optimizable (%lu)",
          (unsigned long)d.usage);
    CHECK(d.storageMode == MTLStorageModeManaged && d.resourceOptions == MTLResourceStorageModeManaged && d.cpuCacheMode == 0 &&
              d.hazardTrackingMode == 0, "managed (Metal's default on macOS), packed in the options: %#lx, storage %lu",
          (unsigned long)d.resourceOptions, (unsigned long)d.storageMode);
    d.resourceOptions = MTLResourceStorageModePrivate | MTLResourceCPUCacheModeWriteCombined | MTLResourceHazardTrackingModeUntracked;
    CHECK(d.storageMode == MTLStorageModePrivate && d.cpuCacheMode == MTLCPUCacheModeWriteCombined &&
              d.hazardTrackingMode == MTLHazardTrackingModeUntracked, "options set: each mode read from them");
    d.storageMode = MTLStorageModeShared;
    CHECK(d.resourceOptions == (MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined | MTLResourceHazardTrackingModeUntracked),
          "a mode set: the options change with it, the others kept (%#lx)", (unsigned long)d.resourceOptions);
    MTLTextureDescriptor *t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:640
                                                                                height:480 mipmapped:YES];
    CHECK(t.pixelFormat == MTLPixelFormatBGRA8Unorm && t.width == 640 && t.height == 480 && t.mipmapLevelCount == 10,
          "2D, mipmapped: 640 x 480, 10 levels (%lu)", (unsigned long)t.mipmapLevelCount);
    t.storageMode = MTLStorageModePrivate, t.usage = MTLTextureUsageRenderTarget;
    MTLTextureDescriptor *c = [t copy];
    t.width = 1;
    CHECK(c != t && c.width == 640 && c.mipmapLevelCount == 10 && c.resourceOptions == t.resourceOptions &&
              c.storageMode == MTLStorageModePrivate && c.usage == MTLTextureUsageRenderTarget,
          "a copy: its own, with every value");
}

static void test_command_checks(void) {
    printf("guest values Metal would stop the app on (F7)\n");
    /* The tessellation factor buffer: "offset(%lu) must be a multiple of 4 bytes.", and its instanceStride */
    HvObject *b = object(14);
    b->kind = KIND_BUFFER, b->object = [FakeBuffer new];
    struct __attribute__((packed)) { uint32_t ref; uint64_t off, stride; } tess[3] = {{14, 6, 16}, {14, 8, 6}, {14, 8, 16}};
    TessEncoder *te[3];
    for (int i = 0; i < 3; i++) {
        TessEncoder *e = te[i] = [TessEncoder new];
        const void *rec = &tess[i];
        dispatch_sync(r->worker, ^{ [r render:(id<MTLRenderCommandEncoder>)e tag:122 rec:(Rec){rec, 20} task:1]; });
    }
    CHECK(te[0].calls == 1 && !te[0].buffer, "at an offset not a multiple of 4: none bound");
    CHECK(te[1].calls == 1 && !te[1].buffer && te[1].stride == 0, "a stride not a multiple of 4: none bound, no stride");
    CHECK(te[2].buffer == b->object && te[2].offset == 8 && te[2].stride == 16, "aligned: bound as given");

    /* Mipmaps: "tex(%s) is not filterable." Integer formats never filter, 32-bit float ones only where the GPU does */
    FakeTexture *t = [FakeTexture new];
    HvObject *to = object(15);
    to->kind = KIND_TEXTURE, to->object = t;
    static const uint32_t mips[2] = {15, 0};
    struct { NSUInteger format; int float32, made; const char *what; } cases[] = {
        {MTLPixelFormatRGBA8Unorm, 0, 1, "RGBA8Unorm: made"},
        {MTLPixelFormatR32Uint, 1, 0, "R32Uint, an integer format: refused"},
        {MTLPixelFormatRGBA16Sint, 1, 0, "RGBA16Sint: refused"},
        {MTLPixelFormatRGBA32Float, 0, 0, "RGBA32Float on a GPU that doesn't filter it: refused"},
        {MTLPixelFormatRGBA32Float, 1, 1, "RGBA32Float on one that does: made"},
    };
    int float32 = r->caps.float32_filter;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        t.pixelFormat = cases[i].format, r->caps.float32_filter = cases[i].float32;
        MipEncoder *e = [MipEncoder new];
        dispatch_sync(r->worker, ^{ [r blit:(id<MTLBlitCommandEncoder>)e tag:307 rec:(Rec){(const uint8_t *)mips, 4} task:1]; });
        CHECK(e.made == (NSUInteger)cases[i].made, "%s", cases[i].what);
    }
    r->caps.float32_filter = float32;
    dispatch_sync(r->worker, ^{ [r->tasks[1] removeObjectForKey:@14], [r->tasks[1] removeObjectForKey:@15]; });

    /* A resolve into a texture of another size: "texture and resolveTexture size mismatch." */
    HvTexCaps caps = {.samples = 1u << 1 | 1u << 4, .color_bytes = 128, .implicit_bytes = 256 << 10};
    HvPass p;
    memset(&p, 0, sizeof p);
    p.color[0] = attachment(MTLPixelFormatBGRA8Unorm, 64, 4, 1);
    p.color[0].store = MTLStoreActionMultisampleResolve, p.color[0].has_resolve = 1, p.color[0].resolve_texture = 2;
    p.color[0].resolve = attachment(MTLPixelFormatBGRA8Unorm, 64, 1, 2).tex;
    CHECK(!hv_pvg_pass_problem(&p, caps), "resolved into one of its size: begun");
    p.color[0].resolve.width = 128;
    CHECK(hv_pvg_pass_problem(&p, caps), "into a bigger one: dropped");

    /* Depth and stencil in the combined format, on two textures: "...must be the same depth and stencil texture" */
    memset(&p, 0, sizeof p);
    p.depth = attachment(MTLPixelFormatDepth32Float_Stencil8, 64, 1, 3);
    p.stencil = attachment(MTLPixelFormatDepth32Float_Stencil8, 64, 1, 3);
    CHECK(!hv_pvg_pass_problem(&p, caps), "one combined texture for both: begun");
    p.stencil.slice = 0, p.stencil.level = 0, p.stencil.texture = p.stencil.root = 4;
    CHECK(hv_pvg_pass_problem(&p, caps), "two combined textures: dropped");
    p.depth = attachment(MTLPixelFormatDepth32Float, 64, 1, 3);
    p.stencil = attachment(MTLPixelFormatStencil8, 64, 1, 4);
    CHECK(!hv_pvg_pass_problem(&p, caps), "a Depth32Float and a Stencil8 texture: begun");
}

/* The mapper: done at once if the GPU is idle; else after the work before it, each in order, and nothing waits */
static void settle(void) { dispatch_sync(r->worker, ^{}); dispatch_sync(r->mapper_q, ^{}); } /* mapper_done is told from mapper_q */
static void test_fence(void) {
    printf("mapper fence after the GPU's work\n");
    hv_pvg_metal_mapper_fence((__bridge HvPvgMetal *)r, 3, 7);
    settle();
    CHECK(nmapper_done == 1 && mapper_done[0] == 3 && mapper_gen[0] == 7, "GPU idle: done at once, with its generation");
    TestCB *work = exec_ref(6, 2, 20);
    hv_pvg_metal_mapper_fence((__bridge HvPvgMetal *)r, 4, 7);
    settle();
    TestCB *fence = q.made.lastObject;
    CHECK(fence != work && nmapper_done == 1, "work in flight: not yet");
    hv_pvg_metal_mapper_fence((__bridge HvPvgMetal *)r, 5, 8); /* asked after the guest restarted its ring */
    settle();
    CHECK(q.made.lastObject == fence, "the next goes with it: no buffer of its own");
    [work finish:YES];
    settle();
    CHECK(nmapper_done == 1, "not before its own buffer completes");
    [fence finish:YES];
    settle();
    CHECK(nmapper_done == 3 && mapper_done[1] == 4 && mapper_gen[1] == 7 && mapper_done[2] == 5 && mapper_gen[2] == 8 && waits == 0 &&
          atomic_load(&r->inflight) == 0, "then 4, then 5 of the new generation, nothing waited");

    /* (coordinator 3) Not with one from before the guest's or the presenter's work committed since: a buffer of its
       own, after that work. And stamps with no work of their own (finishChannel), however many: one buffer, not one
       each (the queue holds 64, and making the 65th waits) */
    work = exec_ref(6, 2, 21);
    hv_pvg_metal_mapper_fence((__bridge HvPvgMetal *)r, 6, 8);
    settle();
    fence = q.made.lastObject;
    TestCB *work2 = exec_ref(6, 2, 22);
    hv_pvg_metal_mapper_fence((__bridge HvPvgMetal *)r, 7, 8);
    settle();
    TestCB *after_work = q.made.lastObject;
    atomic_fetch_add(&r->presents, 1); /* the presenter commits */
    hv_pvg_metal_mapper_fence((__bridge HvPvgMetal *)r, 8, 8);
    settle();
    TestCB *after_present = q.made.lastObject;
    CHECK(fence != work && after_work != fence && after_work != work2 && after_present != after_work,
          "the guest's work or the presenter's since: a buffer of its own");
    unsigned before = (unsigned)q.made.count, stamps0 = stamps;
    dispatch_sync(r->worker, ^{
        for (uint32_t i = 0; i < 200; i++) [r finishChannel:1 stamp:100 + i];
    });
    CHECK(q.made.count == before, "200 stamps with no work: with the last buffer, none of their own (%lu)", (unsigned long)(q.made.count - before));
    for (TestCB *b in @[work, fence, work2, after_work]) [b finish:YES];
    settle();
    CHECK(nmapper_done == 5 && mapper_done[3] == 6 && mapper_done[4] == 7 && stamps == stamps0 + 2, "each after its work (%u)", nmapper_done);
    [after_present finish:YES];
    settle();
    CHECK(nmapper_done == 6 && stamps == stamps0 + 202 && atomic_load(&r->inflight) == 0, "then the rest, in order");
    CHECK(made_locked == 0, "no buffer made under screen_lock (%u)", made_locked);
}

static HvObject *video_surface;
/* A luma row of a video surface's IOSurface (read in tests with it unlocked: a plain IOSurface's memory stays put) */
static uint8_t *luma_row(IOSurfaceRef io, int row) { return (uint8_t *)IOSurfaceGetBaseAddressOfPlane(io, 0) + IOSurfaceGetBytesPerRowOfPlane(io, 0) * row; }
/* A 2-plane video surface (64x32 luma, 64x16 chroma): the guest's rows into the IOSurface */
static void test_video(void) {
    printf("video surface written after the GPU reads it\n");
    NSDictionary *props = @{(id)kIOSurfaceWidth: @64, (id)kIOSurfaceHeight: @32, (id)kIOSurfacePixelFormat: @((uint32_t)'420v'),
        (id)kIOSurfacePlaneInfo: @[@{(id)kIOSurfacePlaneWidth: @64, (id)kIOSurfacePlaneHeight: @32, (id)kIOSurfacePlaneBytesPerElement: @1},
                                   @{(id)kIOSurfacePlaneWidth: @32, (id)kIOSurfacePlaneHeight: @16, (id)kIOSurfacePlaneBytesPerElement: @2}]};
    HvObject *s = [HvObject new], *tex = object(9);
    HvVideo *v = [HvVideo new];
    v->iosurface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    if (!v->iosurface) { CHECK(0, "no IOSurface"); return; }
    IOSurfaceLock(v->iosurface, 0, NULL);
    for (int i = 0; i < 2; i++)
        memset(IOSurfaceGetBaseAddressOfPlane(v->iosurface, i), 0, IOSurfaceGetBytesPerRowOfPlane(v->iosurface, i) * IOSurfaceGetHeightOfPlane(v->iosurface, i));
    IOSurfaceUnlock(v->iosurface, 0, NULL);
    s->mapped = calloc(1, 0x4000), s->first = 0x100, v->alloc = 64 * 32 + 64 * 16;
    v->shadow = calloc(1, v->alloc);
    v->plane_count = 2, v->plane[0] = (HvVideoPlane){0, 64, 32, 64, 64, 1}, v->plane[1] = (HvVideoPlane){64 * 32, 64, 16, 64, 32, 2};
    v->merged = calloc(1, (size_t)hv_pvg_video_row_bits(v->plane, 2));
    s->video = v;
    tex->surface = s;
    video_surface = s;

    TestCB *reader = exec_ref(9, 0, 30);
    CHECK(s->video->last_reader == (id)reader && r->video_rows == 0, "nothing changed: nothing written, %llu rows", r->video_rows);
    [exec_ref(9, 0, 31) finish:YES], [reader finish:YES];
    CHECK(waits == 0, "nothing changed: no wait for the GPU");
    reader = exec_ref(9, 0, 32);
    s->mapped[0x100 + 64 * 5 + 3] = 0xab, s->mapped[0x100 + 64 * 32 + 64 * 2] = 0xcd; /* luma row 5, chroma row 2 */
    exec_ref(9, 1, 33); /* the guest says it wrote it */
    CHECK(waits == 1 && reader.status == MTLCommandBufferStatusCompleted, "a row changed while the GPU reads it: waited once (%u)", waits);
    IOSurfaceLock(s->video->iosurface, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *y = IOSurfaceGetBaseAddressOfPlane(s->video->iosurface, 0), *c = IOSurfaceGetBaseAddressOfPlane(s->video->iosurface, 1);
    size_t yrow = IOSurfaceGetBytesPerRowOfPlane(s->video->iosurface, 0), crow = IOSurfaceGetBytesPerRowOfPlane(s->video->iosurface, 1);
    CHECK(y[yrow * 5 + 3] == 0xab && c[crow * 2] == 0xcd && r->video_rows == 2, "both rows there, only they written (%llu)", r->video_rows);
    IOSurfaceUnlock(s->video->iosurface, kIOSurfaceLockReadOnly, NULL);
    for (TestCB *b in q.made) if (b.status < MTLCommandBufferStatusCompleted) [b finish:YES];
    s->mapped[0x100 + 64 * 31] = 1;
    exec_ref(9, 1, 34);
    CHECK(waits == 1 && r->video_rows == 3, "the GPU already done: written with no wait");
    for (TestCB *b in q.made) if (b.status < MTLCommandBufferStatusCompleted) [b finish:YES];
}

/* A video surface the guest wrote is uploaded at its next use, whether or not the guest said so. macOS does not always
   say: on the iPad (10 Nov 2026, YouTube in Edge) 55 surfaces changed with no exec's flag 1 or op 0x34 for them, and
   while only the guest's word (or a check once a second) sent rows up, each of those frames showed the surface's old
   picture first: the video jumped back, then forward. An exec's flag 1 and op 0x34's host invalid still send rows up. */
static void test_video_unsaid(void) {
    printf("a video surface the guest wrote goes up at its next use, said or not\n");
    HvObject *s = video_surface;
    CHECK(s, "a video surface from test_video");
    if (!s) return;
    IOSurfaceRef io = s->video->iosurface;
    unsigned long long rows0 = r->video_rows, unsaid0 = r->video_unsaid;
    [exec_ref(9, 0, 200) finish:YES]; /* used, unchanged */
    CHECK(r->video_rows == rows0, "unchanged: nothing goes up (%llu)", r->video_rows - rows0);
    s->mapped[0x100 + 64 * 9] = 0x3f; /* luma row 9, and no word of it from the guest */
    [exec_ref(9, 0, 201) finish:YES];
    CHECK(r->video_rows == rows0 + 1 && luma_row(io, 9)[0] == 0x3f, "written with no word: up at its next use (%llu, %#x)",
          r->video_rows - rows0, luma_row(io, 9)[0]);
    s->mapped[0x100 + 64 * 10] = 0x40;
    [exec_ref(9, 0, 202) finish:YES];
    CHECK(r->video_rows == rows0 + 2 && luma_row(io, 10)[0] == 0x40, "and again at the use after (%llu, %#x)", r->video_rows - rows0,
          luma_row(io, 10)[0]);
    CHECK(r->video_unsaid == unsaid0 + 2, "both counted as uploads with no flag 1 (%llu)", r->video_unsaid - unsaid0);
    s->mapped[0x100 + 64 * 7] = 0x3d;
    [exec_ref(9, 1, 203) finish:YES];
    CHECK(r->video_rows == rows0 + 3 && luma_row(io, 7)[0] == 0x3d, "flag 1: its row up (%llu)", r->video_rows - rows0);
    CHECK(r->video_unsaid == unsaid0 + 2, "and not counted as one with no flag 1 (%llu)", r->video_unsaid - unsaid0);
    s->mapped[0x100 + 64 * 8] = 0x3e;
    uint32_t inv[4] = {1, 1, 9, 1}; /* task 1, one entry: ref 9, host invalid */
    [r invalidate:inv length:sizeof inv];
    [exec_ref(9, 0, 204) finish:YES];
    CHECK(r->video_rows == rows0 + 4, "op 0x34's host invalid: the same (%llu)", r->video_rows - rows0);
    CHECK(atomic_load(&r->inflight) == 0 && waits == 1, "all done, no wait");
}

/* (gate S2, 10 Nov 2026) Metal's completion of work that read a video surface lets it go (last_reader) without waiting
   for video_lock: the worker holds that for a whole surface's compare and upload (about 1 ms each for a 4K frame, at
   every use), and the handler gives the slot back and wakes the presenter only after. Here video_lock is held, as by
   a compare, while the work completes. */
static void test_video_reader_not_behind_compare(void) {
    printf("a video surface's reader completes while its compare holds video_lock\n");
    HvObject *s = video_surface;
    CHECK(s, "a video surface from test_video");
    if (!s) return;
    HvVideo *v = s->video;
    TestCB *reading = exec_ref(9, 0, 210);
    CHECK(v->last_reader == (id)reading, "its newest reader");
    dispatch_semaphore_t held = dispatch_semaphore_create(0), let_go = dispatch_semaphore_create(0), done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ /* the worker's compare */
        os_unfair_lock_lock(&v->video_lock);
        dispatch_semaphore_signal(held);
        dispatch_semaphore_wait(let_go, DISPATCH_TIME_FOREVER);
        os_unfair_lock_unlock(&v->video_lock);
    });
    dispatch_semaphore_wait(held, DISPATCH_TIME_FOREVER);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ /* the GPU finishes meanwhile */
        [reading finish:YES];
        dispatch_semaphore_signal(done);
    });
    long late = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    CHECK(!late && v->last_reader == nil, "completed and let go with video_lock held (%s)", late ? "it waited for the lock" : "done");
    dispatch_semaphore_signal(let_go);
    if (late) dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    CHECK(atomic_load(&r->inflight) == 0, "all done");
}

/* An exec draws into the video surface (flag 2): the guest's bytes are the older from then on, and only those it
   changes go up over what the GPU drew (gate S3: before, none did until the guest's flag 1), whole rows once it says
   it wrote it (flag 1). With sync_after (flag 4) what it drew comes back to the guest's pages on its completion,
   before its stamp. */
static void test_video_gpu_write(void) {
    printf("what the GPU draws into a video surface\n");
    HvObject *s = video_surface;
    CHECK(s, "a video surface from test_video");
    if (!s) return;
    IOSurfaceRef io = s->video->iosurface;
    uint8_t *guest = s->mapped + s->first;
    unsigned long long rows0 = r->video_rows;
    [exec_ref(9, 2, 40) finish:YES];
    CHECK(s->video->host_newer, "drawn into: the host's is the newer");
    IOSurfaceLock(io, 0, NULL); /* the GPU's drawing */
    uint8_t *y = IOSurfaceGetBaseAddressOfPlane(io, 0);
    size_t yrow = IOSurfaceGetBytesPerRowOfPlane(io, 0);
    for (int i = 0; i < 32; i++) memset(y + yrow * i, 0x60, 64);
    IOSurfaceUnlock(io, 0, NULL);
    [exec_ref(9, 0, 41) finish:YES];
    IOSurfaceLock(io, kIOSurfaceLockReadOnly, NULL);
    CHECK(y[yrow * 7] == 0x60 && r->video_rows == rows0, "read by the next exec: the GPU's drawing kept (%llu rows went up)", r->video_rows - rows0);
    IOSurfaceUnlock(io, kIOSurfaceLockReadOnly, NULL);
    guest[64 * 7 + 1] = 0x5b; /* the guest's CPU writes a byte of it, with no word, the drawing not back */
    [exec_ref(9, 0, 44) finish:YES];
    IOSurfaceLock(io, kIOSurfaceLockReadOnly, NULL);
    CHECK(s->video->host_newer && y[yrow * 7 + 1] == 0x5b && y[yrow * 7] == 0x60 && y[yrow * 7 + 2] == 0x60 && r->video_rows == rows0 + 1,
          "its byte goes up at the next use, onto the GPU's drawing, the rest of the row kept (%#x %#x, %llu rows)", y[yrow * 7],
          y[yrow * 7 + 1], r->video_rows - rows0);
    IOSurfaceUnlock(io, kIOSurfaceLockReadOnly, NULL);
    TestCB *cb = exec_ref(9, 4, 42); /* the guest asks for it back */
    CHECK(guest[64 * 3] != 0x60, "not before the GPU is done");
    unsigned before = stamps;
    [cb finish:YES];
    CHECK(guest[0] == 0x60 && guest[64 * 31 + 63] == 0x60 && guest[64 * 7 + 1] == 0x5b && guest[64 * 7] == 0x60 && stamps == before + 1,
          "back in the guest's pages, with the guest's byte, then its stamp");
    CHECK(s->video->shadow[64 * 31 + 63] == 0x60, "and in the shadow: nothing to send up");
    guest[64 * 9 + 1] = 0x22; /* the guest writes it */
    [exec_ref(9, 1, 43) finish:YES];
    IOSurfaceLock(io, kIOSurfaceLockReadOnly, NULL);
    CHECK(!s->video->host_newer && y[yrow * 9 + 1] == 0x22 && y[yrow * 8] == 0x60 && r->video_rows == rows0 + 2,
          "written by the guest (flag 1): its one changed row goes up (%llu)", r->video_rows - rows0 - 1);
    IOSurfaceUnlock(io, kIOSurfaceLockReadOnly, NULL);
    /* (video gate review) Drawn into again, and the guest writes a row of it with no word: its changed byte goes onto
       the drawing, and its bytes written with the values they had are not seen. Its flag 1 then sends that row whole. */
    [exec_ref(9, 2, 45) finish:YES];
    IOSurfaceLock(io, 0, NULL);
    memset(y + yrow * 10, 0x61, 64);
    IOSurfaceUnlock(io, 0, NULL);
    guest[64 * 10] = 0x33; /* the rest of the row as it was (0x60) */
    [exec_ref(9, 0, 46) finish:YES];
    IOSurfaceLock(io, kIOSurfaceLockReadOnly, NULL);
    CHECK(y[yrow * 10] == 0x33 && y[yrow * 10 + 1] == 0x61, "merged: the changed byte, the GPU's in the rest");
    IOSurfaceUnlock(io, kIOSurfaceLockReadOnly, NULL);
    [exec_ref(9, 1, 47) finish:YES];
    IOSurfaceLock(io, kIOSurfaceLockReadOnly, NULL);
    CHECK(!memcmp(y + yrow * 10, guest + 64 * 10, 64) && !s->video->merged_any,
          "its flag 1: the merged row whole, the guest's (%#x)", y[yrow * 10 + 1]);
    IOSurfaceUnlock(io, kIOSurfaceLockReadOnly, NULL);
    /* (video gate re-review, SHOULD A) Merged onto drawing that then never comes back (the GPU work asked to copy it
       back fails): its rows are the GPU's but for the bytes the guest changed. All of it counts as back all the same,
       and that must not drop the marks: the row goes up whole at the next use. */
    [exec_ref(9, 2, 48) finish:YES];
    IOSurfaceLock(io, 0, NULL);
    memset(y + yrow * 11, 0x62, 64);
    IOSurfaceUnlock(io, 0, NULL);
    guest[64 * 11] = 0x34; /* the rest of the row as it was (0x60) */
    [exec_ref(9, 0, 49) finish:YES];
    uint64_t failed0 = r->failed;
    [exec_ref(9, 4, 50) finish:NO]; /* asked back; failed: nothing copied (backs_none) */
    atomic_store(&r->gpu_faulted, 0); /* the breaker's own test is test_breaker */
    CHECK(r->failed == failed0 + 1 && s->video->back_gen == s->video->write_gen && guest[64 * 11 + 1] == 0x60,
          "failed: counted as back, nothing copied (%#x)", guest[64 * 11 + 1]);
    unsigned long long rows1 = r->video_rows;
    [exec_ref(9, 0, 51) finish:YES];
    IOSurfaceLock(io, kIOSurfaceLockReadOnly, NULL);
    CHECK(!memcmp(y + yrow * 11, guest + 64 * 11, 64) && !s->video->merged_any && r->video_rows == rows1 + 1,
          "the next use: the merged row whole, the guest's (%#x, %llu rows)", y[yrow * 11 + 1], r->video_rows - rows1);
    IOSurfaceUnlock(io, kIOSurfaceLockReadOnly, NULL);
    /* (re-review NIT 1) Merged, then all the drawing really copied back: the marked row is already the guest's, so the
       next use writes nothing and counts no upload, nor one macOS wrote without saying */
    [exec_ref(9, 2, 52) finish:YES];
    IOSurfaceLock(io, 0, NULL);
    memset(y + yrow * 12, 0x63, 64);
    IOSurfaceUnlock(io, 0, NULL);
    guest[64 * 12] = 0x35;
    [exec_ref(9, 4, 53) finish:YES]; /* merged at its use, then copied back */
    CHECK(s->video->merged_any && guest[64 * 12 + 1] == 0x63, "merged, then back (%#x)", guest[64 * 12 + 1]);
    unsigned long long uploads1 = r->video_uploads, unsaid1 = r->video_unsaid;
    rows1 = r->video_rows;
    [exec_ref(9, 0, 54) finish:YES];
    CHECK(!s->video->merged_any && r->video_rows == rows1 && r->video_uploads == uploads1 && r->video_unsaid == unsaid1,
          "the next use: unmarked, nothing written or counted (%llu uploads, %llu unsaid)", r->video_uploads - uploads1,
          r->video_unsaid - unsaid1);
    CHECK(atomic_load(&r->inflight) == 0, "all done");
}

/* What comes back is what the GPU drew, and only that: a row the guest's CPU wrote while the work ran is kept (before,
   every row came back over it). Once all the GPU drew is back, the host's copy is no longer the newer, so what the guest
   writes after goes up even with no flag 1 (before, nothing did until a flag 1); not while newer drawing is still
   to come back (the generation it was asked with). */
static void test_video_back(void) {
    printf("what comes back from a video surface, and when the guest's goes up again\n");
    HvObject *s = video_surface;
    CHECK(s, "a video surface from test_video");
    if (!s) return;
    IOSurfaceRef io = s->video->iosurface;
    uint8_t *guest = s->mapped + s->first;
    TestCB *cb = exec_ref(9, 2 | 4, 60); /* draws into it and asks for it back */
    IOSurfaceLock(io, 0, NULL);
    memset(luma_row(io, 14), 0x71, 64); /* the GPU draws row 14 */
    IOSurfaceUnlock(io, 0, NULL);
    guest[64 * 15 + 2] = 0x5c; /* meanwhile the guest's CPU writes row 15 */
    unsigned long long back_rows = r->video_back_rows;
    [cb finish:YES];
    CHECK(guest[64 * 14] == 0x71 && guest[64 * 14 + 63] == 0x71, "the GPU's row back in the guest's pages");
    CHECK(guest[64 * 15 + 2] == 0x5c, "the row the guest wrote meanwhile kept (%#x)", guest[64 * 15 + 2]);
    CHECK(r->video_back_rows == back_rows + 1, "only the row the GPU changed copied back (%llu)", r->video_back_rows - back_rows);
    CHECK(s->video->host_newer, "the host's the newer until the next exec looks");

    unsigned long long rows0 = r->video_rows;
    [exec_ref(9, 0, 61) finish:YES]; /* no flag 1: the guest's row 15 goes up all the same, all the GPU drew being back */
    IOSurfaceLock(io, kIOSurfaceLockReadOnly, NULL);
    CHECK(!s->video->host_newer && luma_row(io, 15)[2] == 0x5c && luma_row(io, 14)[0] == 0x71 && r->video_rows == rows0 + 1,
          "all of it back: the guest's row goes up with no flag 1 (%llu rows), the GPU's kept", r->video_rows - rows0);
    IOSurfaceUnlock(io, kIOSurfaceLockReadOnly, NULL);

    TestCB *first = exec_ref(9, 2 | 4, 62), *second = exec_ref(9, 2, 63); /* drawn into again before the first is back */
    [first finish:YES];
    guest[64 * 16] = 0x3e;
    rows0 = r->video_rows;
    TestCB *third = exec_ref(9, 0, 64);
    CHECK(s->video->host_newer && r->video_rows == rows0 + 1 && luma_row(io, 16)[0] == 0x3e && second.status == MTLCommandBufferStatusCompleted,
          "newer drawing not back yet: the guest's byte goes up onto it once that drawing is done (%llu rows)", r->video_rows - rows0);
    [second finish:YES], [third finish:YES];
    guest[64 * 16] = 0;
    CHECK(atomic_load(&r->inflight) == 0, "all done");

    /* (coordinator 1) Drawn into again while the earlier drawing's copy-back is still to come, the GPU not even done
       with it yet (so not on complete_q either): that copy-back is made first, before this exec is committed, else it
       would copy the surface back as this one half drew it */
    TestCB *drawing = exec_ref(9, 2 | 4, 65);
    IOSurfaceLock(io, 0, NULL);
    memset(luma_row(io, 17), 0x47, 64);
    IOSurfaceUnlock(io, 0, NULL);
    CHECK(atomic_load(&s->video->backs_due) == 1, "its copy-back counted (%d)", atomic_load(&s->video->backs_due));
    unsigned waits0 = waits;
    TestCB *next = exec_ref(9, 2, 66);
    CHECK(drawing.status == MTLCommandBufferStatusCompleted && guest[64 * 17] == 0x47 && waits == waits0 + 1 &&
          atomic_load(&s->video->backs_due) == 0, "drawn into again: the copy-back still to come made first (%u waits)", waits - waits0);
    [next finish:YES];
    exec_ref(9, 0, 67);
    CHECK(waits == waits0 + 1, "read only: no wait for it");
    for (TestCB *b in q.made) if (b.status < MTLCommandBufferStatusCompleted) [b finish:YES];

    /* (coordinator 4) A failed exec's copy-backs never come: the surface is not left behind for them for good (every
       later exec waiting, and the guest's own writing never going up) */
    uint64_t failed0 = atomic_load(&r->failed);
    TestCB *failing = exec_ref(9, 2 | 4, 68);
    [failing finishWithError:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal userInfo:nil]];
    CHECK(atomic_load(&r->failed) == failed0 + 1 && !atomic_load(&r->gpu_faulted), "failed once: told, not stopped");
    CHECK(s->video->back_gen == s->video->write_gen && atomic_load(&s->video->backs_due) == 0,
          "none to come: not behind (%llu of %llu)", s->video->back_gen, s->video->write_gen);
    guest[64 * 18 + 5] = 0x19;
    rows0 = r->video_rows;
    [exec_ref(9, 0, 69) finish:YES];
    CHECK(!s->video->host_newer && luma_row(io, 18)[5] == 0x19 && r->video_rows == rows0 + 1,
          "the guest's writing goes up again (%llu rows)", r->video_rows - rows0);
    [r gpuRearm]; /* the strike forgotten */
    CHECK(atomic_load(&r->inflight) == 0, "all done");
}

/* Two processes map the same guest memory as a video surface under their own mapping ids (WebKit's GPU process draws
   a tile, WindowServer reads it): one host side for both (HvVideo), so what one draws is what the other reads, with one
   shadow, one count of the drawing still to come back and one newest reader. Another layout of the same pages has its
   own. Each mapping is its own map of the guest's pages, unmapped once when it goes; the host side, which holds no
   guest memory, stays for a later mapping. */
static HvObject *map_surface(uint32_t ref, uint64_t mapping) {
    hv_pvg_metal_map((__bridge HvPvgMetal *)r, (uint32_t)mapping); /* the guest maps it first (the mapper's op 1) */
    dispatch_sync(r->worker, ^{});
    HvObject *o = object(ref);
    memcpy(surface_record, &mapping, 8);
    [r makeMappedTexture:o at:0x8000 task:1];
    return o->surface;
}
static void describe_surface(uint8_t *d, uint32_t first) {
    const uint32_t list = (0x10000 >> 12) | 1, fourcc = '420v';
    const uint64_t alloc = 64 * 32 + 64 * 16, w = 64, h = 32, row = 64;
    memcpy(d, &list, 4), memcpy(d + 4, &fourcc, 4), memcpy(d + 0x18, &alloc, 8), memcpy(d + 0x20, &w, 8), memcpy(d + 0x28, &h, 8);
    memcpy(d + 0x30, &row, 8), memcpy(d + 0x60, &first, 4);
    d[0x3e] = 2;
    const uint64_t planes[2][5] = {{0, 64 * 32, 64, 32, 64}, {64 * 32, 64 * 16, 32, 16, 64}}; /* offset, size, width, height, row */
    const uint16_t bpe[2] = {1, 2};
    for (int i = 0; i < 2; i++) {
        uint8_t *p = d + 0x80 + i * 0x80;
        memcpy(p + 8, &planes[i][0], 8), memcpy(p + 0x18, &planes[i][1], 8), memcpy(p + 0x20, &planes[i][2], 8);
        memcpy(p + 0x28, &planes[i][3], 8), memcpy(p + 0x30, &planes[i][4], 8), memcpy(p + 0x38, &bpe[i], 2);
    }
}
static int no_room_left(void *ctx, uint64_t bytes, char *why, size_t why_len) {
    (void)ctx, (void)bytes;
    snprintf(why, why_len, "the test's: no room");
    return -1;
}
/* (gate r1, S3-2) A surface page not on a 16 KB boundary: copied into the surface mapping and counted, never the 16 KB
   page below it (vm_remap truncates the source), whose bytes would show shifted by 8 KB, and never left as zeros. */
static void test_surface_misaligned(void) {
    printf("a surface page not on a 16 KB boundary: copied, counted\n");
    vm_address_t ram = 0;
    if (vm_allocate(mach_task_self(), &ram, 0x8000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { CHECK(0, "no guest memory"); return; }
    guest_ram = (uint8_t *)ram;
    const uint32_t page = (0x22000 >> 12) | 1; /* 8 KB past a boundary (bit 0 is the entry's present bit) */
    memcpy(guest_ram, &page, 4);
    describe_surface(surface_desc[0], 0x100);
    const uint32_t tag = 0xc, size = 0x30;
    memcpy(surface_record + 8, &tag, 4), memcpy(surface_record + 12, &size, 4);
    guest_ram[0x4000 + 0x100 + 64 * 3] = 0x41; /* the 16 KB page below: what a truncated remap would show */
    guest_ram[0x6000 + 0x100 + 64 * 3] = 0x52; /* the guest's own page, copied instead of zeroed */
    r->planar = [NSMapTable strongToWeakObjectsMapTable], r->planar_kept = [NSMutableArray new], r->surfaces = [NSMutableDictionary new], r->max_buffer = 1ull << 30;
    uint64_t misaligned0 = r->pages_misaligned;
    @autoreleasepool {
        HvObject *s = map_surface(11, 5);
        CHECK(s && s->mapped && s->mapped[0x100 + 64 * 3] == 0x52 && r->pages_misaligned == misaligned0 + 1,
              "copied from guest RAM (%02x, not page below 0x41), and counted (%llu)",
              s && s->mapped ? s->mapped[0x100 + 64 * 3] : 0xff,
              r->pages_misaligned - misaligned0);
        guest_ram[0x6000 + 0x100 + 64 * 3] = 0x79;
        sync_copied_surface(s);
        CHECK(s && s->mapped && s->mapped[0x100 + 64 * 3] == 0x79, "guest write refreshed copied misaligned page (%02x)",
              s && s->mapped ? s->mapped[0x100 + 64 * 3] : 0xff);
        s->guest_stale = YES;
        guest_ram[0x6000 + 0x100 + 64 * 3] = 0x99;
        sync_copied_surface(s);
        CHECK(s && s->mapped && s->mapped[0x100 + 64 * 3] == 0x79, "stale surface not overwritten by guest RAM (%02x)",
              s && s->mapped ? s->mapped[0x100 + 64 * 3] : 0xff);
        s = nil;
        hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 5);
        [r->tasks[1] removeObjectForKey:@11];
        settle();
    }
    CHECK(ngpu_mapped == 0 && gpu_bad_unmaps == 0, "unmapped once");
    [r->planar_kept removeAllObjects], r->planar_kept_bytes = 0;
    guest_ram = NULL;
    vm_deallocate(mach_task_self(), ram, 0x8000);
}
/* (gate r1, S3-1 and 705) The guest's pages are pinned before they are remapped: a page the guest gives back (the
   balloon) meanwhile is zeroed where it is, never swapped for new memory under the GPU (core/ramfile.c). A buffer's
   mapping that fails unpins them; a surface with a run that failed lists its pages again without that run's (its srcs
   cleared by remap_runs). */
static void test_map_pin_first(void) {
    printf("a mapping: pinned before remapped; a failed one unpinned; a surface's failed run not listed\n");
    vm_address_t ram = 0;
    if (vm_allocate(mach_task_self(), &ram, 32 << 14, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { CHECK(0, "no guest memory"); return; }
    task3_ram = (uint8_t *)ram;
    for (int i = 0; i < 4; i++) task3_pages[i] = 0x100 + i; /* a run */
    task3_pages[4] = 0x110;                                  /* another */
    r->max_buffer = 1ull << 30;
    unsigned remaps0 = vm_remaps, maps0 = gpu_maps, unmaps0 = gpu_unmaps, mapped0 = ngpu_mapped, bad0 = gpu_bad_unmaps;
    HvObject *o = [HvObject new];
    o->length = 5 << 14;
    CHECK(map_pages(r, 3, o) && gpu_maps == maps0 + 1 && gpu_map_remaps[maps0 % 64] == remaps0 && gpu_map_listed[maps0 % 64] == 5,
          "pinned before the remaps (%u made before it), every page listed", gpu_map_remaps[maps0 % 64] - remaps0);
    if (o->mapped) {
        test_gpu_unmap(o->mapped);
        vm_deallocate(mach_task_self(), (vm_address_t)o->mapped, 5 << 14);
    }
    free(o->pages), o->pages = NULL, o->mapped = NULL;
    /* A remap that fails: not mapped, and what was pinned unpinned */
    maps0 = gpu_maps, unmaps0 = gpu_unmaps;
    HvObject *bad = [HvObject new];
    bad->length = 5 << 14;
    vm_remap_fails = 1;
    CHECK(!map_pages(r, 3, bad) && !bad->mapped, "a remap failed: not mapped");
    vm_remap_fails = 0;
    CHECK(gpu_maps == maps0 + 1 && gpu_unmaps == unmaps0 + 1 && ngpu_mapped == mapped0 && gpu_bad_unmaps == bad0,
          "pinned, then unpinned (%u maps, %u unmaps)", gpu_maps - maps0, gpu_unmaps - unmaps0);
    task3_ram = NULL;
    vm_deallocate(mach_task_self(), ram, 32 << 14);

    /* A surface whose one run fails: zeros, pinned first, then listed again without it */
    if (vm_allocate(mach_task_self(), &ram, 0x8000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { CHECK(0, "no guest memory"); return; }
    guest_ram = (uint8_t *)ram;
    const uint32_t page = (0x20000 >> 12) | 1;
    memcpy(guest_ram, &page, 4);
    describe_surface(surface_desc[0], 0x100);
    const uint32_t tag = 0xc, size = 0x30;
    memcpy(surface_record + 8, &tag, 4), memcpy(surface_record + 12, &size, 4);
    guest_ram[0x4000 + 0x100] = 0x63;
    r->planar = [NSMapTable strongToWeakObjectsMapTable], r->planar_kept = [NSMutableArray new], r->surfaces = [NSMutableDictionary new];
    remaps0 = vm_remaps, maps0 = gpu_maps, unmaps0 = gpu_unmaps;
    unsigned relists0 = gpu_relists;
    @autoreleasepool {
        vm_remap_fails = 1, vm_remap_fail_lists = 1; /* and no memory for a second page list */
        HvObject *s = map_surface(12, 6);
        vm_remap_fails = 0, vm_remap_fail_lists = 0, gpu_list_fails = 0;
        CHECK(s && s->mapped && s->mapped[0x100] == 0, "the failed run: zeros (%02x)", s && s->mapped ? s->mapped[0x100] : 0xff);
        CHECK(gpu_maps == maps0 + 1 && gpu_map_remaps[maps0 % 64] == remaps0 && gpu_map_listed[maps0 % 64] == 1,
              "pinned before the remap (%u maps, %u remaps before)", gpu_maps - maps0, gpu_map_remaps[maps0 % 64] - remaps0);
        CHECK(gpu_relists == relists0 + 1 && gpu_relist_listed == 0 && gpu_unmaps == unmaps0,
              "listed again without the failed run, in one step (%u relists, %u listed, %u unmaps)", gpu_relists - relists0,
              gpu_relist_listed, gpu_unmaps - unmaps0);
        /* The pages stay pinned while mapped, even with no memory for the new list (a map then an unmap would have
           dropped the only list) */
        CHECK(ngpu_mapped == mapped0 + 1, "still pinned while mapped (%u mappings)", ngpu_mapped - mapped0);
        s = nil;
        hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 6);
        [r->tasks[1] removeObjectForKey:@12];
        settle();
    }
    CHECK(ngpu_mapped == mapped0 && gpu_bad_unmaps == bad0, "unmapped once");
    [r->planar_kept removeAllObjects], r->planar_kept_bytes = 0;
    guest_ram = NULL;
    vm_deallocate(mach_task_self(), ram, 0x8000);
}
static void test_video_shared(void) {
    printf("one host surface for every mapping of the same guest memory\n");
    vm_address_t ram = 0;
    if (vm_allocate(mach_task_self(), &ram, 0x8000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { CHECK(0, "no guest memory"); return; }
    guest_ram = (uint8_t *)ram;
    const uint32_t page = (0x20000 >> 12) | 1;
    memcpy(guest_ram, &page, 4);
    uint8_t *px = guest_ram + 0x4000 + 0x100; /* the surface's pixels, as the guest's CPU sees them */
    describe_surface(surface_desc[0], 0x100), describe_surface(surface_desc[1], 0x200);
    const uint32_t tag = 0xc, size = 0x30;
    memcpy(surface_record + 8, &tag, 4), memcpy(surface_record + 12, &size, 4);
    r->planar = [NSMapTable strongToWeakObjectsMapTable], r->planar_kept = [NSMutableArray new], r->surfaces = [NSMutableDictionary new], r->max_buffer = 1ull << 30;
    px[64 * 3] = 0x41;
    unsigned maps0 = gpu_maps, unmaps0 = gpu_unmaps;
    unsigned long long rows0 = r->video_rows;
    HvVideo *v;
    @autoreleasepool {
        HvObject *a = map_surface(11, 5);
        CHECK(a && a->video, "a new surface: its host side made");
        unsigned long long rows_a = r->video_rows;
        HvObject *b = map_surface(12, 6), *c = map_surface(13, 7);
        v = a ? a->video : nil;
        CHECK(v && b && b != a && b->video == v && c && c->video && c->video != v && r->planar.count == 2,
              "the same memory and layout: one host side; from another offset: its own (%lu)", (unsigned long)r->planar.count);
        if (!v || !b || b->video != v) return;
        CHECK(gpu_maps == maps0 + 3 && b->mapped != a->mapped, "each mapping its own map of the guest's pages (%u)", gpu_maps - maps0);
        CHECK(rows_a == rows0 + 1 && r->video_rows == rows_a && luma_row(v->iosurface, 3)[0] == 0x41,
              "the guest's row went up once, with the first mapping (%llu, %llu)", rows_a - rows0, r->video_rows - rows_a);

        TestCB *drawn = exec_ref(12, 2 | 4, 70); /* drawn into through b, and asked back */
        IOSurfaceLock(v->iosurface, 0, NULL);
        memset(luma_row(v->iosurface, 20), 0x77, 64);
        IOSurfaceUnlock(v->iosurface, 0, NULL);
        px[64 * 21] = 0x12; /* meanwhile the guest's CPU writes row 21 */
        unsigned long long rows1 = r->video_rows;
        TestCB *reading = exec_ref(11, 0, 71); /* read through a */
        CHECK(r->video_rows == rows1 + 1 && luma_row(v->iosurface, 21)[0] == 0x12 && luma_row(v->iosurface, 20)[0] == 0x77 &&
              drawn.status == MTLCommandBufferStatusCompleted,
              "b's drawing not back: the guest's byte goes up through a once b's drawing is done, the drawing kept (%llu rows)",
              r->video_rows - rows1);
        [drawn finish:YES];
        CHECK(px[64 * 20] == 0x77 && a->mapped[0x100 + 64 * 20] == 0x77 && px[64 * 21] == 0x12 && v->back_gen == v->write_gen,
              "back in the guest's pages through b, the guest's own row kept");
        unsigned w0 = waits;
        px[64 * 21 + 1] = 0x13;
        exec_ref(12, 1, 72); /* the guest wrote it, through b: a's reader is let finish first */
        CHECK(waits == w0 + 1 && reading.status == MTLCommandBufferStatusCompleted && luma_row(v->iosurface, 21)[1] == 0x13,
              "a's reader waited for, then the guest's row up (%u waits)", waits - w0);
        for (TestCB *x in [q.made copy]) if (x.status < MTLCommandBufferStatusCompleted) [x finish:YES];
        CHECK(v->last_reader == nil, "all done: no reader kept");

        uint32_t both[7] = {1, 2, 0, 11, 2 | 4, 12, 2 | 4}; /* both mappings drawn into and asked back in one exec */
        CHECK([r exec:both length:sizeof both channel:1 stamp:73], "exec taken");
        unsigned long long backs = r->video_backs;
        [q.made.lastObject finish:YES];
        CHECK(r->video_backs == backs + 1 && v->back_gen == v->write_gen, "one copy back for both (%llu)", r->video_backs - backs);
        CHECK(atomic_load(&r->inflight) == 0, "all done");

        /* (review 4) drawn into through b and written by the guest through a, in one exec: the guest's rows go up
           first, the GPU's drawing is the newer after, and comes back */
        uint32_t mixed[7] = {1, 2, 0, 12, 2 | 4, 11, 1};
        CHECK([r exec:mixed length:sizeof mixed channel:1 stamp:73], "exec taken");
        CHECK(v->host_newer, "the host's the newer");
        IOSurfaceLock(v->iosurface, 0, NULL);
        memset(luma_row(v->iosurface, 22), 0x33, 64);
        IOSurfaceUnlock(v->iosurface, 0, NULL);
        [q.made.lastObject finish:YES];
        CHECK(px[64 * 22] == 0x33 && v->back_gen == v->write_gen, "and back in the guest's pages");
        CHECK(atomic_load(&r->inflight) == 0, "all done");

        /* (gfx-render 2) Two surfaces drawn into and asked back in one exec: both back in the guest's pages before its
           stamp, however much that is. Not the old trigger itself (a budget for copy-backs used up by earlier ones put
           off, which can't be set or happen now): the rule that replaced it, that nothing is put off past its stamp. */
        double back_time = r->video_back_time;
        HvVideo *w = c->video;
        uint8_t *wpx = c->mapped + 0x200; /* w's pixels as c's mapping has them */
        uint32_t two[7] = {1, 2, 0, 11, 2 | 4, 13, 2 | 4};
        CHECK([r exec:two length:sizeof two channel:1 stamp:74], "exec taken");
        IOSurfaceLock(v->iosurface, 0, NULL), IOSurfaceLock(w->iosurface, 0, NULL);
        memset(luma_row(v->iosurface, 23), 0x44, 64), memset(luma_row(w->iosurface, 9), 0x5a, 64);
        IOSurfaceUnlock(v->iosurface, 0, NULL), IOSurfaceUnlock(w->iosurface, 0, NULL);
        __block BOOL both_at_stamp = NO;
        on_stamp = ^(uint32_t stamp) {
            if (stamp == 74) both_at_stamp = px[64 * 23] == 0x44 && wpx[64 * 9] == 0x5a;
        };
        backs = r->video_backs;
        [q.made.lastObject finish:YES];
        on_stamp = nil;
        CHECK(both_at_stamp, "both back in the guest's pages when it has the stamp");
        CHECK(r->video_backs == backs + 2 && v->back_gen == v->write_gen && w->back_gen == w->write_gen, "both copied back (%llu)",
              r->video_backs - backs);
        CHECK(r->video_back_time > back_time, "and the time they took tallied");
        CHECK(atomic_load(&r->inflight) == 0, "all done");

        /* (review of 4, M1) synchronize: drawn into with no ask back (flag 2), then asked for by synchronize, whose
           stamp comes from finishChannel (a trail after the synchronize buffer), not from a completion of its own */
        uint32_t drawn_only[5] = {1, 1, 0, 11, 2};
        CHECK([r exec:drawn_only length:sizeof drawn_only channel:1 stamp:75], "exec taken");
        IOSurfaceLock(v->iosurface, 0, NULL);
        memset(luma_row(v->iosurface, 24), 0x66, 64);
        IOSurfaceUnlock(v->iosurface, 0, NULL);
        [q.made.lastObject finish:YES];
        CHECK(v->host_newer && px[64 * 24] != 0x66, "drawn, not asked back: not in the guest's pages yet");
        NSUInteger made = q.made.count;
        const uint32_t sync[3] = {1, 1, 11};
        [r synchronize:sync length:sizeof sync];
        [r finishChannel:1 stamp:76];
        CHECK(q.made.count == made + 2, "the synchronize's buffer, and a trail for the stamp (%lu)", (unsigned long)(q.made.count - made));
        __block BOOL synced_at_stamp = NO, stamped = NO;
        on_stamp = ^(uint32_t stamp) {
            if (stamp == 76) stamped = YES, synced_at_stamp = px[64 * 24] == 0x66;
        };
        for (NSUInteger i = made; i < q.made.count; i++) [q.made[i] finish:YES];
        on_stamp = nil;
        CHECK(stamped && synced_at_stamp, "synchronize: back in the guest's pages when it has the stamp");
        CHECK(v->back_gen == v->write_gen && atomic_load(&r->inflight) == 0, "all done");
        a = b = c = nil, drawn = reading = nil;
        [q.made removeAllObjects];
        hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 5);
        [r->tasks[1] removeObjectForKey:@11];
        settle();
    }
    CHECK(gpu_unmaps == unmaps0 + 1 && gpu_bad_unmaps == 0 && r->surfaces[@6]->video == v, "a's mapping gone, once; b's kept, and the host side");
    @autoreleasepool {
        hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 6), hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 7);
        [r->tasks[1] removeObjectForKey:@12], [r->tasks[1] removeObjectForKey:@13];
        settle();
    }
    CHECK(gpu_unmaps == unmaps0 + 3 && gpu_bad_unmaps == 0 && ngpu_mapped == 0, "every mapping unmapped once (%u)", gpu_unmaps - unmaps0);
    px[64 * 25] = 0x29; /* the guest's CPU writes the memory while no mapping has it (planar_keep holds the host side) */
    @autoreleasepool {
        HvObject *again = map_surface(14, 6); /* mapped again later (another process) */
        CHECK(again && again->video == v && r->planar.count == 2 && luma_row(v->iosurface, 20)[0] == 0x77,
              "a later mapping finds the same host side, with what was drawn");
        CHECK(luma_row(v->iosurface, 25)[0] == 0x29, "picked up again: what the guest wrote meanwhile goes up at once (%u)",
              luma_row(v->iosurface, 25)[0]);
        again = nil;
        hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 6);
        [r->tasks[1] removeObjectForKey:@14];
        settle();
    }
    CHECK(ngpu_mapped == 0 && gpu_bad_unmaps == 0, "and unmapped once");
    /* An unmapped surface's pages are the guest's to reuse: a texture over it the guest still names is not used (made
       anew), and none is made from its id's descriptor until the guest maps it again */
    @autoreleasepool {
        HvObject *s = map_surface(15, 6), *tex = r->tasks[1][@15];
        CHECK(s && tex->surface == s && !s->gone, "mapped again");
        table[15][0] = OBJ_MAPPED_TEXTURE | 0x30 << 8, table[15][1] = 0x8000, table[15][2] = 0; /* as the guest's entry has it */
        memcpy(tex->entry, table[15], 12);
        hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 6);
        settle();
        CHECK(s->gone && !r->surfaces[@6], "unmapped: gone");
        __block HvObject *now = nil;
        dispatch_sync(r->worker, ^{ now = [r object:15 task:1]; });
        CHECK(now && now != tex && !now->surface && !now->object && now->gone && r->tasks[1][@15] == now,
              "a texture over it still named: made anew, and refused");
        HvObject *fresh = object(10);
        const uint64_t six = 6;
        memcpy(surface_record, &six, 8);
        [r makeMappedTexture:fresh at:0x8000 task:1];
        CHECK(!fresh->surface && !fresh->object && fresh->gone && !r->surfaces[@6], "a new texture over the unmapped id: refused");
        HvObject *again = map_surface(10, 6);
        CHECK(again && r->surfaces[@6] == again && !again->gone, "once the guest maps it again: made");
        hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 6);
        hv_pvg_metal_mapper_reset((__bridge HvPvgMetal *)r);
        settle();
        CHECK(no_unmapped(), "the guest's ring started again: none held unmapped");
        /* The mapper's state handed over whole (the Metal side made, or a restore): what it holds unmapped is refused */
        uint8_t bits[HV_PVG_MAPPING_IDS / 8] = {0};
        bits[0] = 1u << 6 | 1u; /* 6, and id 0, which is none */
        hv_pvg_metal_set_unmapped((__bridge HvPvgMetal *)r, bits);
        settle();
        CHECK(is_unmapped(6) && !is_unmapped(0), "handed over: 6 unmapped (and 0 is no id)");
        HvObject *handed = object(10);
        [r makeMappedTexture:handed at:0x8000 task:1];
        CHECK(!handed->surface && handed->gone && !r->surfaces[@6], "a texture over an id the mapper holds unmapped: refused");
        memset(bits, 0, sizeof bits);
        hv_pvg_metal_set_unmapped((__bridge HvPvgMetal *)r, bits);
        settle();
        CHECK(no_unmapped(), "handed over again, none unmapped: none held");
        handed = nil;
        s = tex = now = fresh = again = nil;
        [r->tasks[1] removeObjectForKey:@15], [r->tasks[1] removeObjectForKey:@10];
        settle();
    }
    CHECK(ngpu_mapped == 0 && gpu_bad_unmaps == 0, "every mapping unmapped once");
    guest_ram = NULL;
    vm_deallocate(mach_task_self(), ram, 0x8000);
}

/* (review 4) planar: a host side still in use is never let go, however many others come and go meanwhile (once 512
   of them or 1 GB were, all were: the next mapping of a tile WebKit still drew into got a new one, which WindowServer
   showed blank); one no longer mapped only while it is among the most recent (HV_PVG_PLANAR_KEPT) */
static void test_video_kept(void) {
    printf("a video surface still in use keeps its host side\n");
    vm_address_t ram = 0;
    if (vm_allocate(mach_task_self(), &ram, 0x8000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { CHECK(0, "no guest memory"); return; }
    guest_ram = (uint8_t *)ram;
    const uint32_t page = (0x20000 >> 12) | 1;
    memcpy(guest_ram, &page, 4);
    describe_surface(surface_desc[0], 0x100), describe_surface(surface_desc[1], 0x200);
    const uint32_t tag = 0xc, size = 0x30;
    memcpy(surface_record + 8, &tag, 4), memcpy(surface_record + 12, &size, 4);
    r->planar = [NSMapTable strongToWeakObjectsMapTable], r->planar_kept = [NSMutableArray new], r->planar_kept_bytes = 0;
    r->surfaces = [NSMutableDictionary new], r->max_buffer = 1ull << 30;
    __weak HvVideo *first_other = nil;
    @autoreleasepool {
        HvObject *a = map_surface(11, 5); /* WebKit's GPU process maps its tile */
        HvVideo *v = a ? a->video : nil;
        CHECK(v != nil, "a host side");
        for (uint32_t i = 0; i < 600; i++) @autoreleasepool { /* 600 other tiles of 1 MB made and unmapped since */
            HvVideo *x = [HvVideo new];
            x->held = 1 << 20;
            const uint64_t k[2] = {0xdead0000ull + i, i};
            planar_add(r, [NSData dataWithBytes:k length:sizeof k], x);
            planar_keep(r, x);
            if (!i) first_other = x;
        }
        CHECK(first_other == nil && r->planar_kept.count <= HV_PVG_PLANAR_KEPT_MAX && r->planar_kept_bytes <= HV_PVG_PLANAR_KEPT,
              "the oldest of those let go (%lu kept, %llu MB)", (unsigned long)r->planar_kept.count, r->planar_kept_bytes >> 20);
        CHECK(r->planar.count <= 2 * HV_PVG_PLANAR_KEPT_MAX + 64, "planar rid of their keys (%lu)", (unsigned long)r->planar.count);
        HvObject *b = map_surface(12, 6); /* WindowServer maps the same tile now */
        CHECK(b && v && b->video == v, "still mapped: the same host side for the next mapping of it");
        a = b = nil, v = nil;
        hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 5), hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 6);
        [r->tasks[1] removeObjectForKey:@11], [r->tasks[1] removeObjectForKey:@12];
        settle();
    }
    CHECK(ngpu_mapped == 0 && gpu_bad_unmaps == 0, "every mapping unmapped once");
    __weak HvVideo *unmapped = r->planar_kept.lastObject;
    CHECK(unmapped != nil, "no longer mapped: held a while");
    @autoreleasepool {
        for (uint32_t i = 0; i < 300; i++) @autoreleasepool { /* 300 MB more unmapped since */
            HvVideo *x = [HvVideo new];
            x->held = 1 << 20;
            const uint64_t k[2] = {0xbeef0000ull + i, i};
            planar_add(r, [NSData dataWithBytes:k length:sizeof k], x);
            planar_keep(r, x);
        }
    }
    CHECK(unmapped == nil && r->planar_kept_bytes <= (256ull << 20),
          "then let go: no more than 256 MB held for surfaces no longer mapped (%llu MB)", r->planar_kept_bytes >> 20);
    [r->planar_kept removeAllObjects], r->planar_kept_bytes = 0;
    guest_ram = NULL;
    vm_deallocate(mach_task_self(), ram, 0x8000);
}

/* (review 4) Which mappings share a host side: their page lists compared entry by entry, each a present page's address
   (as d3bb8ba compared the first's, bit 0 masked) or none: an absent page's other bits are not an address, and two
   mappings both missing it reach the same bytes there (none). A page one has and the other hasn't is not the same
   memory: shared, the one without would put its zeros up over what the other's has. */
static void test_video_key(void) {
    printf("which mappings share a host side\n");
    vm_address_t ram = 0;
    if (vm_allocate(mach_task_self(), &ram, 0xc000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { CHECK(0, "no guest memory"); return; }
    guest_ram = (uint8_t *)ram;
    const uint32_t absent5 = 0x40 << 2, absent7 = 0x80 << 2, present = (0x20000 >> 12) | 1; /* bit 0 clear: no page */
    memcpy(guest_ram, &absent5, 4), memcpy(guest_ram + 0x8000, &absent7, 4);
    guest_ram[0x4000 + 0x100 + 64 * 3] = 0x41;
    describe_surface(surface_desc[0], 0x100), describe_surface(surface_desc[1], 0x100);
    const uint32_t list7 = (0x30000 >> 12) | 1;
    memcpy(surface_desc[1], &list7, 4);
    const uint32_t tag = 0xc, size = 0x30;
    memcpy(surface_record + 8, &tag, 4), memcpy(surface_record + 12, &size, 4);
    r->planar = [NSMapTable strongToWeakObjectsMapTable], r->planar_kept = [NSMutableArray new], r->planar_kept_bytes = 0;
    r->surfaces = [NSMutableDictionary new], r->max_buffer = 1ull << 30;
    @autoreleasepool {
        HvObject *a = map_surface(11, 5), *c = map_surface(13, 7);
        CHECK(a && c && a->video && a->video == c->video, "both without the page, whatever else its entries say: shared");
        a = c = nil;
        hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 5), hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 7);
        [r->tasks[1] removeObjectForKey:@11], [r->tasks[1] removeObjectForKey:@13];
        settle();
    }
    [r->planar_kept removeAllObjects], r->planar_kept_bytes = 0;
    memcpy(guest_ram, &present, 4);
    @autoreleasepool {
        HvObject *a = map_surface(11, 5), *c = map_surface(13, 7);
        CHECK(a && c && a->video && c->video && a->video != c->video && luma_row(a->video->iosurface, 3)[0] == 0x41,
              "one with the page, one without: each its own, and the one with keeps its pixels");
        a = c = nil;
        hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 5), hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 7);
        [r->tasks[1] removeObjectForKey:@11], [r->tasks[1] removeObjectForKey:@13];
        settle();
    }
    CHECK(ngpu_mapped == 0 && gpu_bad_unmaps == 0, "every mapping unmapped once");
    [r->planar_kept removeAllObjects], r->planar_kept_bytes = 0;
    guest_ram = NULL;
    vm_deallocate(mach_task_self(), ram, 0xc000);
}

/* (review 4) An exec dropped for want of memory for its segments changes nothing first: a video surface it would draw
   into is not made the host's newer, nor counted drawn, and nothing of it is encoded on a command buffer never sent */
static void test_exec_no_memory(void) {
    printf("an exec dropped for want of memory changes nothing\n");
    vm_address_t ram = 0;
    if (vm_allocate(mach_task_self(), &ram, 0x8000, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { CHECK(0, "no guest memory"); return; }
    guest_ram = (uint8_t *)ram;
    const uint32_t page = (0x20000 >> 12) | 1;
    memcpy(guest_ram, &page, 4);
    describe_surface(surface_desc[0], 0x100);
    const uint32_t tag = 0xc, size = 0x30;
    memcpy(surface_record + 8, &tag, 4), memcpy(surface_record + 12, &size, 4);
    r->planar = [NSMapTable strongToWeakObjectsMapTable], r->planar_kept = [NSMutableArray new], r->planar_kept_bytes = 0;
    r->surfaces = [NSMutableDictionary new], r->max_buffer = 1ull << 30;
    @autoreleasepool {
        HvObject *a = map_surface(11, 5);
        HvVideo *v = a ? a->video : nil;
        CHECK(v && !v->host_newer, "a host side, the guest's the newest");
        uint64_t gen = v ? v->write_gen : 0;
        seg_data_fails = 1;
        uint32_t drawn[5] = {1, 1, 0, 11, 2};
        BOOL took = [r exec:drawn length:sizeof drawn channel:1 stamp:80];
        seg_data_fails = 0;
        CHECK(!took && v && !v->host_newer && v->write_gen == gen, "dropped: not drawn into (%d, %llu)", v ? v->host_newer : -1,
              v ? v->write_gen - gen : 0);
        HvObject *t = r->tasks[1][@11];
        CHECK(t && !t->guest_stale, "nor the guest's copy out of date");
        /* (review) dropped while the GPU is stopped, an exec saying the guest wrote it: kept for a Try Again */
        if (v) v->host_newer = YES;
        atomic_store(&r->gpu_faulted, 1);
        uint32_t wrote[5] = {1, 1, 0, 11, 1};
        took = [r exec:wrote length:sizeof wrote channel:1 stamp:81];
        atomic_store(&r->gpu_faulted, 0);
        CHECK(!took && v && !v->host_newer, "dropped, the guest wrote it: its copy the newer (%d)", v ? v->host_newer : -1);
        a = nil, v = nil;
        hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 5);
        [r->tasks[1] removeObjectForKey:@11];
        settle();
    }
    [r->planar_kept removeAllObjects], r->planar_kept_bytes = 0;
    guest_ram = NULL;
    vm_deallocate(mach_task_self(), ram, 0x8000);
}

/* GPU work that comes back failed (a hang the GPU's watchdog stopped, a page fault) stops the guest's GPU work for the
   rest of the run: its execs are still taken, their stamps given back in order, and nothing more is committed. A
   refusal for an app in the background (NotPermitted) isn't the guest's doing and trips nothing. */
static void test_breaker(void) {
    printf("GPU fault breaker\n");
    HvPvgMetal *m = (__bridge HvPvgMetal *)r;
    CHECK(!hv_pvg_metal_gpu_faulted(m), "not tripped to start with");
    [exec_ref(6, 2, 50) finishWithError:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorNotPermitted userInfo:nil]];
    CHECK(!hv_pvg_metal_gpu_faulted(m), "NotPermitted: not tripped");
    TestCB *a = exec_ref(6, 2, 51), *b = exec_ref(6, 2, 52);
    unsigned before = stamps, made = (unsigned)q.made.count;
    [a finishWithError:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorTimeout userInfo:nil]];
    CHECK(hv_pvg_metal_gpu_faulted(m) && stamps == before + 1, "a timeout: tripped, its stamp given back");
    uint32_t e[5] = {1, 1, 0, 6, 2};
    BOOL ran = [r exec:e length:sizeof e channel:1 stamp:53];
    CHECK(!ran, "the next exec: not run");
    [r finishChannel:1 stamp:53]; /* as the worker does for an exec not run */
    CHECK(stamps == before + 1, "its stamp after the work still in flight");
    [b finish:YES];
    for (TestCB *x in q.made) if (x.status < MTLCommandBufferStatusCompleted) [x finish:YES];
    CHECK(stamps == before + 3, "then both stamps (%u)", stamps - before);
    CHECK(q.made.count <= made + 1, "nothing of the guest's committed after (only the empty one its stamp waits on): %lu", (unsigned long)(q.made.count - made));
    CHECK(atomic_load(&r->inflight) == 0, "all done");
}

/* Which failures trip the breaker: the guest's own (a timeout, a page fault, a stack overflow) at once; others only
   3 within 10 s (they can come from the host: memory, a GPU restart); NotPermitted (a background iPad app) never.
   The app can re-arm it. */
static NSError *gpu_error(long code) { return [NSError errorWithDomain:MTLCommandBufferErrorDomain code:code userInfo:nil]; }
static void test_breaker_classes(void) {
    printf("GPU fault breaker: which failures trip it\n");
    HvPvgMetal *m = (__bridge HvPvgMetal *)r;
    for (int i = 0; i < 6; i++) [r gpuFailed:gpu_error(MTLCommandBufferErrorNotPermitted)];
    CHECK(!hv_pvg_metal_gpu_faulted(m), "NotPermitted, 6 times: not tripped");
    [r gpuFailed:gpu_error(MTLCommandBufferErrorInternal)];
    [r gpuFailed:gpu_error(MTLCommandBufferErrorOutOfMemory)];
    CHECK(!hv_pvg_metal_gpu_faulted(m), "two failures not the guest's own: not tripped");
    [r gpuFailed:gpu_error(MTLCommandBufferErrorInternal)];
    CHECK(hv_pvg_metal_gpu_faulted(m), "a third within 10 s: tripped");
    hv_pvg_metal_gpu_rearm(m);
    CHECK(!hv_pvg_metal_gpu_faulted(m), "re-armed");
    [r gpuFailed:gpu_error(MTLCommandBufferErrorInternal)];
    CHECK(!hv_pvg_metal_gpu_faulted(m), "re-arming forgets the earlier failures");
    hv_pvg_metal_gpu_rearm(m);
    uint64_t long_ago = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - 20ull * 1000000000ull;
    r->gpu_strikes[0] = r->gpu_strikes[1] = long_ago, r->gpu_strike_next = 2;
    [r gpuFailed:gpu_error(MTLCommandBufferErrorInvalidResource)];
    CHECK(!hv_pvg_metal_gpu_faulted(m), "two failures 20 s ago and one now: not tripped");
    static const long at_once[3] = {MTLCommandBufferErrorTimeout, MTLCommandBufferErrorPageFault, MTLCommandBufferErrorStackOverflow};
    for (int i = 0; i < 3; i++) {
        hv_pvg_metal_gpu_rearm(m);
        [r gpuFailed:gpu_error(at_once[i])];
        CHECK(hv_pvg_metal_gpu_faulted(m), "error %ld: tripped at once", at_once[i]);
    }
    hv_pvg_metal_gpu_rearm(m);
    CHECK(!hv_pvg_metal_gpu_faulted(m) && atomic_load(&r->inflight) == 0, "re-armed, nothing in flight");
}

/* The real presenter (TestReplay's never draws) on a layer with no drawable to give, as a hidden screen's: it goes on
   wanting one while it retries, and once it gives up for a cool-down nothing is waiting, so the freeze recorder's
   drawable stage doesn't grow for as long as the screen stays hidden and still */
@interface NoDrawableLayer : NSObject
@property CGSize drawableSize;
@end
@implementation NoDrawableLayer
- (id)nextDrawable { return nil; }
@end
@interface WaitReplay : HvReplay
@end
@implementation WaitReplay
- (BOOL)prepareScreenForLayer:(CAMetalLayer *)target { (void)target; return YES; } /* no pipelines: no GPU here */
@end
static void test_drawable_wait(void) {
    printf("the presenter's drawable wait\n");
    WaitReplay *w = [WaitReplay new];
    w->screen_lock = OS_UNFAIR_LOCK_INIT;
    w->presenter = dispatch_queue_create("test.wait", DISPATCH_QUEUE_SERIAL);
    dispatch_suspend(w->presenter); /* the retry is queued, not run */
    w->layer = (CAMetalLayer *)[NoDrawableLayer new];
    w->screen_w = w->screen_h = 64, w->screen_dirty = YES, w->presenter_running = YES;
    w->screen_tex = (id)[NSObject new]; /* a screen to show: kickPresenter starts the presenter only with one */
    [w presentLoop];
    CHECK(atomic_load(&w->st_drawable_since) && w->drawable_retry_pending && !w->presenter_running && w->screen_dirty,
          "none given: still wanted, a retry queued");
    w->drawable_retry_pending = NO, w->drawable_retry_deadline = now_seconds() - 1, w->presenter_running = YES;
    [w presentLoop];
    CHECK(!atomic_load(&w->st_drawable_since) && w->drawable_retry_cooldown > 0 && !w->drawable_retry_pending,
          "given up for a cool-down: nothing waits (%llu)", (unsigned long long)atomic_load(&w->st_drawable_since));
    os_unfair_lock_lock(&w->screen_lock);
    w->layer = nil; /* the queued retry finds no layer */
    os_unfair_lock_unlock(&w->screen_lock);
    dispatch_resume(w->presenter);
}

/* At most HV_PVG_GUEST_INFLIGHT (4) of the guest's command buffers committed and not done: a fifth exec waits (on the
   worker, here another thread) until one of them completes, then goes on. */
static void test_inflight_cap(void) {
    printf("the guest's command buffers in flight\n");
    TestCB *first[HV_PVG_GUEST_INFLIGHT];
    for (int i = 0; i < HV_PVG_GUEST_INFLIGHT; i++) first[i] = exec_ref(6, 2, 60 + (uint32_t)i);
    CHECK(atomic_load(&r->inflight) == HV_PVG_GUEST_INFLIGHT, "4 in flight");
    __block _Atomic int fifth_done = 0;
    dispatch_semaphore_t went = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        uint32_t a[5] = {1, 1, 0, 6, 2};
        [r exec:a length:sizeof a channel:1 stamp:64];
        atomic_store(&fifth_done, 1);
        dispatch_semaphore_signal(went);
    });
    long waited = dispatch_semaphore_wait(went, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC));
    CHECK(waited != 0 && !atomic_load(&fifth_done), "a fifth waits while 4 are in flight");
    [first[0] finish:YES];
    waited = dispatch_semaphore_wait(went, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
    CHECK(waited == 0 && atomic_load(&fifth_done), "and goes on once one completes");
    for (TestCB *x in [q.made copy]) if (x.status < MTLCommandBufferStatusCompleted) [x finish:YES];
    CHECK(atomic_load(&r->inflight) == 0, "all done");
}

/* Suspend fails before the memory step while any of the guest's GPU work may still run: a fake that never completes,
   so no real GPU hangs. Each case ends before anything is written (the blob stubs abort). */
static int save_writes;
static int save_write(void *ctx, const void *p, size_t n) { (void)ctx, (void)p, (void)n; save_writes++; return 0; }
static int save_room(void *ctx, uint64_t b, char *why, size_t len) { (void)ctx, (void)b, (void)why, (void)len; return 0; }
static int save_now(char *why, size_t len) {
    HvPvgSink sink = {save_write, NULL, save_room};
    return hv_pvg_metal_save((__bridge HvPvgMetal *)r, &sink, snap_mono_ns() + 30 * NSEC_PER_MSEC, why, len);
}
static int save_on_worker(char *why, size_t len) { /* the worker's own answer, with no outer timeout to cover for it */
    HvSnapJob *job = [HvSnapJob new];
    job->lock = OS_UNFAIR_LOCK_INIT;
    snap_save_on_worker(r, job, snap_mono_ns() + 30 * NSEC_PER_MSEC);
    snprintf(why, len, "%s", job->why);
    return job->rc;
}
static void test_save_waits(void) {
    printf("Suspend with the guest's GPU work unfinished\n");
    const char *late = "the Mac's graphics work didn't finish in time";
    char why[160];
    atomic_store(&r->gpu_faulted, 0); /* as Resume re-arms it after test_breaker */
    TestCB *a = exec_ref(6, 2, 80);
    CHECK(save_now(why, sizeof why) == -1 && !strcmp(why, late), "the last buffer unfinished: fails (%s)", why);
    settle();
    CHECK(save_on_worker(why, sizeof why) == -1 && !strcmp(why, late), "and on the worker (%s)", why);
    TestCB *b = exec_ref(6, 2, 81);
    [b finish:YES];
    CHECK(r->last_cb == (id)b && atomic_load(&r->inflight) == 1, "the last done, an earlier one not");
    CHECK(save_now(why, sizeof why) == -1 && !strcmp(why, late), "one in flight: fails (%s)", why);
    settle();
    CHECK(save_on_worker(why, sizeof why) == -1 && !strcmp(why, late), "and on the worker (%s)", why);
    TestCB *c = exec_ref(6, 2, 82);
    [c finishWithError:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorTimeout userInfo:nil]];
    CHECK(hv_pvg_metal_gpu_faulted((__bridge HvPvgMetal *)r) && atomic_load(&r->inflight) == 1, "tripped, with one still in flight");
    CHECK(save_now(why, sizeof why) == -1 && !strcmp(why, late), "tripped with one in flight: fails (%s)", why);
    settle();
    CHECK(save_on_worker(why, sizeof why) == -1 && !strcmp(why, late), "and on the worker (%s)", why);
    CHECK(save_writes == 0, "nothing written");
    [a finish:YES];
    for (TestCB *x in q.made) if (x.status < MTLCommandBufferStatusCompleted) [x finish:YES];
    settle();
    atomic_store(&r->gpu_faulted, 0);
    CHECK(atomic_load(&r->inflight) == 0, "all done");
}

/* An auto-save's hold: once hv_pvg_metal_hold returns, nothing queued for the worker runs (here: the guest's next exec,
   which would write its pages) until the release; a second hold is refused; work queued ahead of the hold runs first,
   and a hold not parked in time fails and lets the worker go; a release with no hold, or one after the worker went on
   (HV_PVG_HOLD_MAX, here 300 ms), says so. Only the worker: nothing reaches the GPU. */
static void test_hold(void) {
    printf("an auto-save's hold on the worker\n");
    HvPvgMetal *m = (__bridge HvPvgMetal *)r;
    __block _Atomic int ran = 0;
    CHECK(hv_pvg_metal_release(m) == -1, "a release with no hold: -1");
    CHECK(hv_pvg_metal_hold(m, 100 * NSEC_PER_MSEC) == 0, "held");
    CHECK(hv_pvg_metal_hold(m, 100 * NSEC_PER_MSEC) == -1, "a second hold: refused");
    dispatch_async(r->worker, ^{ atomic_store(&ran, 1); }); /* as the guest's next exec comes */
    usleep(100 * 1000);
    CHECK(!atomic_load(&ran), "the worker runs nothing while held");
    CHECK(hv_pvg_metal_release(m) == 0, "released, the worker parked all along");
    dispatch_sync(r->worker, ^{});
    CHECK(atomic_load(&ran), "then it runs what came");
    /* work queued ahead: the hold returns once it is done (parked), or fails past wait_ns */
    dispatch_semaphore_t go = dispatch_semaphore_create(0);
    dispatch_async(r->worker, ^{ dispatch_semaphore_wait(go, DISPATCH_TIME_FOREVER); });
    CHECK(hv_pvg_metal_hold(m, 50 * NSEC_PER_MSEC) == -1 && atomic_load(&r->held) == 0, "busy past wait_ns: not held, let go");
    dispatch_semaphore_signal(go);
    dispatch_sync(r->worker, ^{});
    atomic_store(&ran, 0);
    dispatch_async(r->worker, ^{ usleep(20 * 1000); atomic_store(&ran, 1); });
    CHECK(hv_pvg_metal_hold(m, 1000 * NSEC_PER_MSEC) == 0 && atomic_load(&ran), "busy a little: held once what was ahead ran");
    CHECK(hv_pvg_metal_release(m) == 0, "released");
    /* never released: the worker goes on after HV_PVG_HOLD_MAX, and the release says so */
    atomic_store(&ran, 0);
    CHECK(hv_pvg_metal_hold(m, 100 * NSEC_PER_MSEC) == 0, "held again");
    dispatch_async(r->worker, ^{ atomic_store(&ran, 1); });
    for (int i = 0; i < 200 && !atomic_load(&ran); i++) usleep(10 * 1000);
    CHECK(atomic_load(&ran) && atomic_load(&r->held) == 2, "past the longest hold: the worker went on");
    CHECK(hv_pvg_metal_hold(m, 100 * NSEC_PER_MSEC) == -1, "no new hold until that one is released");
    CHECK(hv_pvg_metal_release(m) == -1, "its release: -1, the save can't count on it");
    CHECK(hv_pvg_metal_hold(m, 100 * NSEC_PER_MSEC) == 0 && hv_pvg_metal_release(m) == 0, "then holds work again");
    CHECK(atomic_load(&r->inflight) == 0, "nothing in flight");
}

/* A command buffer the GPU never finishes (a GPU that is sick: its watchdog should have ended it): the worker waits for
   it only HV_PVG_GPU_WAIT (here 1 s), then goes without what it wanted from it, says so, and takes it once it is
   done. Before, it waited for ever (waitUntilCompleted), and with it every frame of the Mac's. The real cb_wait
   here, on the fakes' status: nothing reaches a GPU. */
#ifdef HV_PVG_TEST_HOOKS
static void test_gpu_never_done(void) {
    printf("a command buffer the GPU never finishes\n");
    cb_wait_test = NULL;
    TestCB *done = [TestCB new], *hung = [TestCB new];
    [done commit], [done finish:YES], hung.hung = YES, [hung commit];
    CHECK(cb_wait(r, nil, HV_PVG_GPU_WAIT, NULL) && cb_wait(r, (id<MTLCommandBuffer>)done, HV_PVG_GPU_WAIT, NULL), "nothing, or done: at once");
    _Atomic int stop = 1;
    double t0 = now_seconds();
    BOOL ok = cb_wait(r, (id<MTLCommandBuffer>)hung, HV_PVG_SNAP_GPU_WAIT, &stop);
    CHECK(!ok && now_seconds() - t0 < 0.5 && !r->stuck_cb, "a save given up meanwhile: no more waiting (and not taken for stuck)");
    t0 = now_seconds();
    ok = cb_wait(r, (id<MTLCommandBuffer>)hung, HV_PVG_GPU_WAIT, NULL);
    double took = now_seconds() - t0;
    CHECK(!ok && took >= 0.9 && took < 5 && r->stuck_cb == (id)hung, "never done: given up after its longest wait (%.0f ms)", took * 1e3);
    TestCB *behind = [TestCB new];
    [behind commit];
    t0 = now_seconds();
    ok = cb_wait(r, (id<MTLCommandBuffer>)behind, HV_PVG_GPU_WAIT, NULL);
    CHECK(!ok && now_seconds() - t0 < 0.5, "one behind it while it is stuck: given up at once");
    [behind finish:YES];
    CHECK(cb_wait(r, (id<MTLCommandBuffer>)behind, HV_PVG_GPU_WAIT, NULL) && r->stuck_cb == (id)hung, "one done: yes, whatever is stuck");
    hung.hung = NO, [hung finish:YES];
    CHECK(cb_wait(r, (id<MTLCommandBuffer>)done, HV_PVG_GPU_WAIT, NULL) && !r->stuck_cb, "the stuck one done: forgotten");
    hung = [TestCB new], hung.hung = YES, [hung commit];
    CHECK(hung_waits == 0, "never a wait without end");

    /* The CPU's copy of the screen (the boot's frames, the Mac's own tests): no frame while the GPU's last work runs */
    os_unfair_lock_lock(&r->screen_lock);
    id layer = r->layer;
    r->layer = nil;
    os_unfair_lock_unlock(&r->screen_lock);
    if (!r->surfaces) r->surfaces = [NSMutableDictionary new];
    HvObject *screen = [HvObject new];
    screen->fourcc = 'BGRA', screen->surface_w = 4, screen->surface_h = 2, screen->row = 16, screen->length = 32;
    screen->mapped = calloc(1, 32);
    screen->mapped[5] = 0x5a;
    r->surfaces[@99] = screen;
    id last = r->last_cb;
    r->last_cb = (id<MTLCommandBuffer>)hung;
    r->last_capture = 0;
    uint64_t frames = r->frames, given_up = atomic_load(&r->gpu_waits_given_up);
    t0 = now_seconds();
    dispatch_sync(r->worker, ^{ [r present:99]; });
    took = now_seconds() - t0;
    CHECK(hung_waits == 0 && took < 5, "the frame: not waited for without end (%.0f ms)", took * 1e3);
    CHECK(r->frames == frames && atomic_load(&r->gpu_waits_given_up) == given_up + 1, "no frame taken, the give-up counted");
    r->last_capture = 0;
    t0 = now_seconds();
    dispatch_sync(r->worker, ^{ [r present:99]; });
    CHECK(r->frames == frames && now_seconds() - t0 < 0.5, "the next frame while it is stuck: skipped at once");
    CHECK([r->complaints containsObject:@"the GPU's last work still running after its longest wait: frames skipped until it is done"],
          "and told");
    hung.hung = NO;
    [hung finish:YES];
    r->last_capture = 0;
    dispatch_sync(r->worker, ^{ [r present:99]; });
    CHECK(!r->stuck_cb, "done: no longer stuck");
    CHECK(r->frames == frames + 1 && r->frame && r->frame[5] == 0x5a, "once it is done: the frame taken, its pixels");
    r->last_cb = last;
    free(screen->mapped), screen->mapped = NULL; /* the test's own memory, not a mapping the object gives back */
    [r->surfaces removeObjectForKey:@99];
    os_unfair_lock_lock(&r->screen_lock);
    r->layer = layer;
    os_unfair_lock_unlock(&r->screen_lock);

    /* A video surface the GPU still reads (test_video's): the guest's new rows wait, not the worker */
    HvObject *s = video_surface;
    if (!s) { CHECK(0, "no video surface"); return; }
    for (TestCB *x in q.made) if (x.status < MTLCommandBufferStatusCompleted) [x finish:YES];
    TestCB *reader = exec_ref(9, 0, 90);
    reader.hung = YES;
    s->mapped[0x100 + 64 * 7] = 0x77; /* luma row 7 */
    uint64_t rows = r->video_rows;
    given_up = atomic_load(&r->gpu_waits_given_up);
    t0 = now_seconds();
    exec_ref(9, 1, 91); /* the guest wrote it (earlier tests leave the GPU's drawing the newer) */
    took = now_seconds() - t0;
    CHECK(hung_waits == 0 && took < 5 && r->video_rows == rows, "its reader never done: not written, not waited for without end (%.0f ms)",
          took * 1e3);
    CHECK(atomic_load(&r->gpu_waits_given_up) == given_up + 1, "the give-up counted");
    reader.hung = NO;
    for (TestCB *x in q.made) if (x.status < MTLCommandBufferStatusCompleted) [x finish:YES];
    exec_ref(9, 1, 92);
    IOSurfaceLock(s->video->iosurface, kIOSurfaceLockReadOnly, NULL);
    const uint8_t *y = IOSurfaceGetBaseAddressOfPlane(s->video->iosurface, 0);
    size_t yrow = IOSurfaceGetBytesPerRowOfPlane(s->video->iosurface, 0);
    CHECK(r->video_rows > rows && y[yrow * 7] == 0x77, "once it is done: the row goes up with the next exec (%llu rows)", r->video_rows - rows);
    IOSurfaceUnlock(s->video->iosurface, kIOSurfaceLockReadOnly, NULL);
    for (TestCB *x in q.made) if (x.status < MTLCommandBufferStatusCompleted) [x finish:YES];
    settle();
    CHECK(atomic_load(&r->inflight) == 0, "all done");
    cb_wait_test = fake_cb_wait;
}
#endif

/* The person's "Try Again" after the breaker tripped (hv_pvg_metal_gpu_try_again): the guest's work runs again; not
   while some of it is still on the GPU (a hang the watchdog has yet to end); and HV_GPU_TRIES (2) times a run, then
   only restarting the Mac does. Before, nothing in the app could bring the graphics back. */
static void test_try_again(void) {
    printf("Try Again after the GPU failed the guest's work\n");
    HvPvgMetal *m = (__bridge HvPvgMetal *)r;
    for (TestCB *x in q.made) if (x.status < MTLCommandBufferStatusCompleted) [x finish:YES];
    atomic_store(&r->gpu_faulted, 0), atomic_store(&r->gpu_tries, 0);
    CHECK(hv_pvg_metal_gpu_state(m) == HV_PVG_GPU_RUNS && hv_pvg_metal_gpu_try_again(m) == -1, "running: nothing to try again");
    [r gpuFailed:gpu_error(MTLCommandBufferErrorTimeout)];
    CHECK(hv_pvg_metal_gpu_state(m) == HV_PVG_GPU_STOPPED, "a hang: stopped, can be tried again");
    uint32_t e[5] = {1, 1, 0, 6, 2};
    unsigned made = (unsigned)q.made.count;
    BOOL ran = [r exec:e length:sizeof e channel:1 stamp:100];
    if (!ran) [r finishChannel:1 stamp:100];
    CHECK(!ran, "meanwhile the guest's work isn't run");
    /* (review) but what a dropped exec says the guest wrote (flag 1) is kept: else, tried again, the object keeps the
       GPU's older copy for good */
    HvObject *wrote = r->tasks[1][@6];
    wrote->guest_stale = YES, wrote->host_valid = YES;
    uint32_t w1[5] = {1, 1, 0, 6, 1};
    if (![r exec:w1 length:sizeof w1 channel:1 stamp:100]) [r finishChannel:1 stamp:100];
    CHECK(!wrote->guest_stale && !wrote->host_valid, "the guest's writing while stopped: kept");
    /* (review) and the uploads a dropped or failed buffer took with it: once tried again, every object the guest's
       memory has the newest copy of goes up again */
    HvObject *kept = object(14);
    kept->guest_backed = YES, kept->host_valid = YES;
    CHECK(hv_pvg_metal_gpu_try_again(m) == 0 && hv_pvg_metal_gpu_state(m) == HV_PVG_GPU_RUNS && !hv_pvg_metal_gpu_faulted(m),
          "tried again: running");
    TestCB *cb = exec_ref(6, 2, 101);
    CHECK(q.made.count > made && cb.status == MTLCommandBufferStatusCommitted, "and its next exec runs on the GPU");
    CHECK(!kept->host_valid, "an object uploaded before: up again when next used");
    [r->tasks[1] removeObjectForKey:@14];
    [r gpuFailed:gpu_error(MTLCommandBufferErrorPageFault)]; /* with cb still on the GPU */
    CHECK(hv_pvg_metal_gpu_state(m) == HV_PVG_GPU_STOPPED_SETTLING && hv_pvg_metal_gpu_try_again(m) == -1,
          "stopped again with work still on the GPU: not yet");
    /* (review) while stopped, the stamps of the execs the guest still sends come back from empty buffers behind the
       presenter's frames: those can't hang, and waiting for them would keep it settling while the guest draws */
    TestCB *frame = [q commandBuffer];
    [frame commit];
    atomic_fetch_add(&r->inflight, 1); /* as the presenter counts its own */
    [r finishChannel:1 stamp:102];
    TestCB *trail = q.made.lastObject;
    CHECK(trail != frame && trail.status == MTLCommandBufferStatusCommitted && hv_pvg_metal_gpu_state(m) == HV_PVG_GPU_STOPPED_SETTLING,
          "a stamp behind a frame, the guest's own buffer still on the GPU: settling");
    [cb finish:YES];
    settle();
    CHECK(trail.status == MTLCommandBufferStatusCommitted && hv_pvg_metal_gpu_state(m) == HV_PVG_GPU_STOPPED,
          "the guest's buffer done, the stamp's still behind the frame: stopped, not settling");
    CHECK(hv_pvg_metal_gpu_state(m) == HV_PVG_GPU_STOPPED && hv_pvg_metal_gpu_try_again(m) == 0, "once it is done: tried again (2 of 2)");
    [frame finish:YES];
    atomic_fetch_sub(&r->inflight, 1);
    [trail finish:YES];
    settle();
    [r gpuFailed:gpu_error(MTLCommandBufferErrorTimeout)];
    CHECK(hv_pvg_metal_gpu_state(m) == HV_PVG_GPU_STOPPED_FOR_GOOD && hv_pvg_metal_gpu_try_again(m) == -1 && hv_pvg_metal_gpu_faulted(m),
          "a third time: stopped for this run, restarting the Mac is the way back");
    hv_pvg_metal_gpu_rearm(m); /* the tests' own re-arm has no limit */
    atomic_store(&r->gpu_tries, 0);
    CHECK(atomic_load(&r->inflight) == 0 && hv_pvg_metal_gpu_state(m) == HV_PVG_GPU_RUNS, "all done");
    /* (review) a commit waiting for a slot, all taken by work the GPU hangs on: once one of them fails (the breaker),
       it stops waiting, uncommitted, rather than for the rest of them */
    dispatch_semaphore_t slots = [r guestSlots];
    for (int i = 0; i < HV_PVG_GUEST_INFLIGHT; i++) dispatch_semaphore_wait(slots, DISPATCH_TIME_FOREVER);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_global_queue(0, 0), ^{
        atomic_store(&r->gpu_faulted, 1);
    });
    TestCB *waits = [q commandBuffer];
    double t0 = now_seconds();
    BOOL took = [r commit:(id<MTLCommandBuffer>)waits channel:0 stamp:0 complete:NO redraw:NO];
    CHECK(!took && waits.status == MTLCommandBufferStatusNotEnqueued && now_seconds() - t0 < 3,
          "waiting for a slot: given up once the GPU failed, uncommitted");
    for (int i = 0; i < HV_PVG_GUEST_INFLIGHT; i++) dispatch_semaphore_signal(slots);
    [r gpuRearm];
    CHECK(atomic_load(&r->inflight) == 0 && hv_pvg_metal_gpu_state(m) == HV_PVG_GPU_RUNS, "and running again");
}

/* The app's build (this file built without HV_PVG_TEST_HOOKS: test_pvg_present_app) with HV_PVG_ENCODE_ONLY set: it
   fails closed. None of the guest's work is committed, its stamps still come back, and re-arming changes nothing. */
static void test_refused(void) {
    printf("HV_PVG_ENCODE_ONLY in a build that runs the guest's work\n");
    HvPvgMetal *m = (__bridge HvPvgMetal *)r;
    object(6);
    unsigned before = stamps;
    uint32_t a[5] = {1, 1, 0, 6, 2};
    BOOL ran = [r exec:a length:sizeof a channel:1 stamp:1];
    if (!ran) [r finishChannel:1 stamp:1]; /* as the worker does for an exec not run */
    CHECK(!ran && stamps == before + 1, "the exec: not run, its stamp given back");
    CHECK(hv_pvg_metal_gpu_faulted(m), "shown as stopped");
    [r gpuRearm];
    ran = [r exec:a length:sizeof a channel:1 stamp:2];
    if (!ran) [r finishChannel:1 stamp:2];
    CHECK(!ran && stamps == before + 2, "re-armed: still not run");
    TestCB *cb = [q commandBuffer];
    CHECK(![r commit:(id<MTLCommandBuffer>)cb channel:0 stamp:0 complete:NO redraw:NO], "a commit of the guest's: refused");
    BOOL committed = NO;
    for (TestCB *x in q.made) committed |= x.status != MTLCommandBufferStatusNotEnqueued;
    CHECK(!committed && atomic_load(&r->inflight) == 0, "nothing committed");
    CHECK(hv_pvg_metal_gpu_state(m) == HV_PVG_GPU_STOPPED_FOR_GOOD && hv_pvg_metal_gpu_try_again(m) == -1,
          "the app is told: stopped for good, no Try Again");
}

/* A command buffer for clampPass: its blit encoder keeps the fences it was told to wait for; its compute encoder
   takes what it is given. Nothing is committed: the device only makes the buffers and builds the clamp. */
@interface FenceBlit : NSObject
@property(nonatomic, strong) NSMutableArray *waited;
@end
@implementation FenceBlit
- (void)waitForFence:(id)f { [_waited addObject:f]; }
- (void)copyFromBuffer:(id)a sourceOffset:(NSUInteger)o toBuffer:(id)b destinationOffset:(NSUInteger)d size:(NSUInteger)n {}
- (void)endEncoding {}
@end
@interface FenceCompute : NSObject
@end
@implementation FenceCompute
- (void)setComputePipelineState:(id)p {}
- (void)setBuffer:(id)b offset:(NSUInteger)o atIndex:(NSUInteger)i {}
- (void)setBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {}
- (void)dispatchThreads:(MTLSize)g threadsPerThreadgroup:(MTLSize)t {}
- (void)endEncoding {}
@end
@interface FenceCB : NSObject
@property(nonatomic, strong) FenceBlit *blit;
@end
@implementation FenceCB
- (id)blitCommandEncoder { _blit = [FenceBlit new]; _blit.waited = [NSMutableArray new]; return _blit; }
- (id)computeCommandEncoder { return [FenceCompute new]; }
@end

/* The indirect draws' clamp copies their arguments before the pass: it waits for every fence the pass waits for,
   those the pass also updates among them (another encoder's work before may write the arguments) */
static void test_clamp_fences(void) {
    printf("the clamp's copy waits for the pass's fences\n");
    id<MTLDevice> dev = getenv("HV_NO_GPU") ? nil : MTLCreateSystemDefaultDevice();
    if (!dev) { printf("  no Metal here\n"); return; }
    r->device = dev;
    id f1 = [dev newFence], f2 = [dev newFence];
    HvObject *a = object(13), *b = object(14), *c = object(15);
    a->kind = KIND_FENCE, a->object = f1;
    b->kind = KIND_FENCE, b->object = f2;
    c->kind = KIND_BUFFER, c->object = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    uint8_t recs[256];
    uint32_t n = 0;
    struct { uint32_t tag, size, ref; } fence[3] = {{25, 12, 13}, {25, 12, 14}, {24, 12, 14}}; /* wait f1, wait f2, update f2 */
    memcpy(recs, fence, sizeof fence), n += sizeof fence;
    struct __attribute__((packed)) { uint32_t tag, size, ref; uint16_t primitive, unused; uint64_t off; } draw = {16, 24, 15, 0, 0, 0};
    memcpy(recs + n, &draw, sizeof draw), n += sizeof draw;
    HvSeg seg = {recs, recs + n, 0, 0, 0};
    FenceCB *cb = [FenceCB new];
    [r clampPass:&seg count:1 from:0 at:recs task:1 commandBuffer:(id<MTLCommandBuffer>)cb];
    CHECK(r->pass_indirect.count == 1, "the indirect draw found");
    NSArray *w = cb.blit.waited;
    CHECK(w.count == 2 && [w containsObject:f1] && [w containsObject:f2], "the copy waited for %lu fences, not both", (unsigned long)w.count);
    r->pass_indirect = nil, r->pass_args = nil;
    r->device = nil;
}

/* (review 4) No more of an exec's indirect draws clamped than HV_PVG_EXEC_INDIRECT (here 3), over all its passes: each
   pass may have 65536, and the 64 MB an exec may copy would hold 2.8 million (200 MB of arguments and jobs) */
static void test_clamp_exec_cap(void) {
    printf("an exec's indirect draws: no more clamped than its cap\n");
    id<MTLDevice> dev = getenv("HV_NO_GPU") ? nil : MTLCreateSystemDefaultDevice();
    if (!dev) { printf("  no Metal here\n"); return; }
    r->device = dev;
    HvObject *c = object(15);
    c->kind = KIND_BUFFER, c->object = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    uint8_t recs[64];
    struct __attribute__((packed)) { uint32_t tag, size, ref; uint16_t primitive, unused; uint64_t off; } draw = {16, 24, 15, 0, 0, 0};
    memcpy(recs, &draw, sizeof draw), memcpy(recs + sizeof draw, &draw, sizeof draw);
    HvSeg seg = {recs, recs + 2 * sizeof draw, 0, 0, 0};
    r->exec_indirect = 0, r->exec_left = nil;
    [r clampPass:&seg count:1 from:0 at:recs task:1 commandBuffer:(id<MTLCommandBuffer>)[FenceCB new]];
    CHECK(r->pass_indirect.count == 2, "the first pass's two (%lu)", (unsigned long)r->pass_indirect.count);
    [r clampPass:&seg count:1 from:0 at:recs task:1 commandBuffer:(id<MTLCommandBuffer>)[FenceCB new]];
    CHECK(r->pass_indirect.count == 1 && [r->complaints containsObject:@"an exec's indirect draws past the clamp's: the rest dropped"],
          "the second's: one, the rest dropped and told (%lu)", (unsigned long)r->pass_indirect.count);
    r->pass_indirect = nil, r->pass_args = nil, r->exec_left = nil;
    r->device = nil;
}

/* A shader in a slot of another kind is refused before Metal builds the pipeline (pipeline_stage_problem): a check of
   what the guest sent. That such a shader stopped the iPad's GPU driver on 9 Nov 2026 is one unproven reading of those
   stops. Metal's own descriptors, with stand-in functions: nothing here reaches a GPU. */
@interface TestFunction : NSObject
@property (nonatomic) MTLFunctionType functionType;
@property (nonatomic, copy) NSString *name;
@end
@implementation TestFunction
@end
static TestFunction *test_function(MTLFunctionType t, NSString *name) {
    TestFunction *f = [TestFunction new];
    f.functionType = t, f.name = name;
    return f;
}

/* A device whose pipelines are described by the test and take 20 ms to build, as a back-end compile does */
@interface SlowPipelineDevice : NSObject
@property(nonatomic, strong) MTLRenderPipelineDescriptor *described;
@end
@implementation SlowPipelineDevice
- (id)newRenderPipelineDescriptorWithSerializedData:(dispatch_data_t)d deserializationContext:(id)c { (void)d, (void)c; return [_described copy]; }
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)e {
    (void)d, (void)e;
    usleep(20000);
    return [NSObject new];
}
@end

/* gfx-cost 3, first step: the pipelines built on the worker are counted and timed, with the longest exec, in the tally */
static void test_pipeline_timed(void) {
    printf("pipelines built: counted and timed\n");
    SlowPipelineDevice *dev = [SlowPipelineDevice new];
    dev.described = [MTLRenderPipelineDescriptor new];
    dev.described.vertexFunction = (id)test_function(MTLFunctionTypeVertex, @"vs");
    dev.described.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id saved_dev = r->device;
    uint32_t saved_rec[7];
    memcpy(saved_rec, sampler_record, sizeof saved_rec);
    r->device = (id)dev;
    r->pipelines_built = 0, r->pipeline_time = r->pipeline_max = 0;
    uint32_t rec[7] = {0xe, 28, 40, 12, 0, 0, 0}; /* {tag, size, ref, length, 12 bytes the fake doesn't read} */
    memcpy(sampler_record, rec, sizeof rec);
    __block HvKind kind = KIND_NONE;
    __block id made = nil;
    dispatch_sync(r->worker, ^{
        made = [r makeSerialized:0x9000 length:28 task:1 kind:&kind into:[HvObject new]];
        made = made ? [r makeSerialized:0x9000 length:28 task:1 kind:&kind into:[HvObject new]] : nil;
    });
    CHECK(made && kind == KIND_RENDER_PIPELINE, "built");
    CHECK(r->pipelines_built == 2 && r->pipeline_time >= 0.04 && r->pipeline_max >= 0.02 && r->pipeline_max < r->pipeline_time,
          "two built, 20 ms each: %llu, %.3f s, longest %.3f s", r->pipelines_built, r->pipeline_time, r->pipeline_max);
    memcpy(sampler_record, saved_rec, sizeof saved_rec);
    r->device = saved_dev;
}

/* gfx-commands F4 (item 5a): a heap buffer placed where its heap can't hold it is refused, with nothing made in its
   place (it was a buffer of its own, up to the heap's size, for every such entry) */
static unsigned own_buffers, heap_placed;
@interface HeapDevice : NSObject
@end
@implementation HeapDevice
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)n options:(MTLResourceOptions)o { (void)o; return (MTLSizeAndAlign){(n + 0xff) & ~0xffull, 0x100}; }
- (id)newBufferWithLength:(NSUInteger)n options:(MTLResourceOptions)o { (void)n, (void)o; own_buffers++; return [NSObject new]; }
@end
@interface RefusingHeap : NSObject
@end
@implementation RefusingHeap
- (NSUInteger)size { return 1 << 20; }
- (id)newBufferWithLength:(NSUInteger)n options:(MTLResourceOptions)o offset:(NSUInteger)at { (void)n, (void)o, (void)at; heap_placed++; return nil; }
@end
static void test_heap_buffer_refused(void) {
    printf("a heap buffer the heap can't hold: refused, nothing made instead\n");
    id saved_dev = r->device;
    uint32_t saved_rec[7];
    memcpy(saved_rec, sampler_record, sizeof saved_rec);
    r->device = (id)[HeapDevice new];
    HvObject *heap = object(11);
    heap->object = [RefusingHeap new], heap->kind = KIND_HEAP;
    own_buffers = heap_placed = 0;
    uint64_t refused0 = r->heap_buffers_refused;
    struct __attribute__((packed)) { uint32_t heap; uint64_t offset, length; } at[2] = {{11, 0x10, 0x1000}, {11, 0x1000, 0x1000}};
    for (int i = 0; i < 2; i++) { /* misaligned; then where it fits, and the heap itself says no */
        memset(sampler_record, 0, sizeof sampler_record), memcpy(sampler_record, &at[i], sizeof at[i]);
        HvObject *o = [HvObject new];
        dispatch_sync(r->worker, ^{ [r makeHeapBuffer:o at:0x9000 task:1]; });
        CHECK(!o->object && o->parent == heap, "%s: no object", i ? "the heap's refusal" : "misaligned");
    }
    CHECK(heap_placed == 1 && own_buffers == 0 && r->heap_buffers_refused == refused0 + 2, "the heap asked once, no buffer of its own (%u), counted",
          own_buffers);
    [task_objects(r, 1, YES) removeObjectForKey:@11];
    memcpy(sampler_record, saved_rec, sizeof saved_rec);
    r->device = saved_dev;
}

/* Item 5a: what the guest asks about an object there is none of (refused, or not the kind asked about) is answered
   with a null address or ID, exactly its length, not left as whatever the guest's buffer held, and kept for a restore */
static void test_info_null(void) {
    printf("info about no object: answered 0, and kept\n");
    vm_address_t ram = 0;
    if (vm_allocate(mach_task_self(), &ram, 32 << 14, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { CHECK(0, "no guest memory"); return; }
    task3_ram = (uint8_t *)ram;
    task3_pages[0] = 0x100;
    r->max_buffer = 1ull << 30;
    static const uint32_t none[3] = {14 | 20 << 8, 0x3200, 0}, bare[3] = {0x77 | 12 << 8, 3, 0};
    memcpy(task3_table[2], none, 12), memcpy(task3_table[3], bare, 12);
    __block BOOL ok = NO;
    dispatch_sync(r->worker, ^{
        HvObject *to = [HvObject new];
        uint32_t e[3] = {1 | 16 << 8, 0x3100, 0};
        memcpy(to->entry, e, 12);
        to->length = 1 << 14, to->kind = KIND_BUFFER, to->object = (id)[NSObject new];
        ok = map_pages(r, 3, to);
        HvObject *refused = [HvObject new], *plain = [HvObject new]; /* no object; one without a resource ID */
        memcpy(refused->entry, none, 12), memcpy(plain->entry, bare, 12);
        plain->kind = KIND_TEXTURE, plain->object = [NSObject new];
        task_objects(r, 3, YES);
        r->tasks[3][@1] = to, r->tasks[3][@2] = refused, r->tasks[3][@3] = plain;
    });
    CHECK(ok, "mapped");
    memset(task3_ram, 0xab, 0x400);
    static const struct { uint32_t tag, ref, at, len; } asked[] = {{461, 2, 0x40, 8}, {463, 2, 0x80, 8}, {462, 2, 0xc0, 16}, {466, 2, 0x100, 16},
                                                                   {460, 2, 0x140, 8}, {462, 3, 0x180, 16}};
    NSMutableDictionary *saved_answers = r->answers;
    uint32_t saved_count = r->answers_count;
    r->answers = [NSMutableDictionary new], r->answers_count = 0; /* as the device makes it */
    NSUInteger kept0 = 0;
    for (unsigned i = 0; i < sizeof asked / sizeof *asked; i++) {
        uint32_t rec[5];
        uint64_t off = asked[i].at;
        if (asked[i].tag == 460) rec[0] = asked[i].ref, rec[1] = 0, rec[2] = 1, memcpy(&rec[3], &off, 8);
        else rec[0] = asked[i].ref, rec[1] = 1, memcpy(&rec[2], &off, 8);
        uint32_t size = asked[i].tag == 460 ? 20 : 16;
        const uint8_t *p = (const uint8_t *)rec;
        dispatch_sync(r->worker, ^{ [r info:asked[i].tag rec:(Rec){p, size} task:3]; });
        BOOL zero = YES, rest = YES;
        for (uint32_t k = 0; k < 32; k++) {
            uint8_t b = task3_ram[asked[i].at + k];
            if (k < asked[i].len) zero = zero && b == 0;
            else if (k < 64 - 8) rest = rest && b == 0xab;
        }
        CHECK(zero && rest, "info %u of ref %u: %u bytes of 0, nothing past them", asked[i].tag, asked[i].ref, asked[i].len);
    }
    CHECK(r->answers[@3].count == kept0 + 6, "each kept, for a restore to ask again (%lu)", (unsigned long)(r->answers[@3].count - kept0));
    dispatch_sync(r->worker, ^{
        r->answers = saved_answers, r->answers_count = saved_count;
        HvObject *to = r->tasks[3][@1];
        for (uint32_t ref = 1; ref <= 3; ref++) [r->tasks[3] removeObjectForKey:@(ref)];
        test_gpu_unmap(to->mapped);
        vm_deallocate(mach_task_self(), (vm_address_t)to->mapped, to->length);
        free(to->pages), to->pages = NULL, to->mapped = NULL;
    });
    memset(task3_table, 0, sizeof task3_table), memset(task3_pages, 0, sizeof task3_pages);
    vm_deallocate(mach_task_self(), ram, 32 << 14);
    task3_ram = NULL;
}

static void test_stage_kinds(void) {
    printf("a pipeline's shaders each in a slot of their own kind\n");
    id vs = test_function(MTLFunctionTypeVertex, @"vs"), fs = test_function(MTLFunctionTypeFragment, @"fs");
    id k = test_function(MTLFunctionTypeKernel, @"k"), vis = test_function(MTLFunctionTypeVisible, @"helper");
    id hit = test_function(MTLFunctionTypeIntersection, @"hit");
    MTLRenderPipelineDescriptor *rp = [MTLRenderPipelineDescriptor new];
    CHECK(!pipeline_stage_problem(rp, 0), "an empty render pipeline: no shader of a wrong kind (a missing vertex shader is refused by hv_pvg_pipeline_problem: test_pipeline_checks)");
    rp.vertexFunction = vs, rp.fragmentFunction = fs;
    CHECK(!pipeline_stage_problem(rp, 0), "vertex and fragment in their slots: built");
    rp.fragmentFunction = nil;
    CHECK(!pipeline_stage_problem(rp, 0), "no fragment shader (drawing nothing): built");
    rp.vertexFunction = fs;
    NSString *w = pipeline_stage_problem(rp, 0);
    CHECK(w && [w containsString:@"vertex"] && [w containsString:@"fs"] && [w containsString:@"fragment"],
          "a fragment shader as the vertex one (the iPad's crash): refused, %s", w.UTF8String ?: "(nil)");
    rp.vertexFunction = vs, rp.fragmentFunction = vs;
    CHECK(pipeline_stage_problem(rp, 0), "a vertex shader as the fragment one: refused");
    rp.fragmentFunction = k;
    CHECK(pipeline_stage_problem(rp, 0), "a kernel as the fragment one: refused");
    rp.fragmentFunction = fs;
    MTLLinkedFunctions *l = [MTLLinkedFunctions new];
    l.functions = @[vis, hit];
    rp.fragmentLinkedFunctions = l;
    CHECK(!pipeline_stage_problem(rp, 0), "visible and intersection helpers: built");
    MTLLinkedFunctions *bad = [MTLLinkedFunctions new];
    bad.functions = @[vis, vs];
    rp.vertexLinkedFunctions = bad;
    CHECK(pipeline_stage_problem(rp, 0), "a vertex shader among the helpers: refused");
    rp.vertexLinkedFunctions = nil;
    MTLLinkedFunctions *binary = [MTLLinkedFunctions new];
    binary.binaryFunctions = @[fs];
    rp.fragmentLinkedFunctions = binary;
    CHECK(pipeline_stage_problem(rp, 0), "a fragment shader among the binary helpers: refused");
    MTLLinkedFunctions *grouped = [MTLLinkedFunctions new];
    grouped.groups = @{@"g": @[k]};
    rp.fragmentLinkedFunctions = grouped;
    CHECK(pipeline_stage_problem(rp, 0), "a kernel in a helper group: refused");

    MTLComputePipelineDescriptor *cp = [MTLComputePipelineDescriptor new];
    cp.computeFunction = k;
    CHECK(!pipeline_stage_problem(cp, 2), "a kernel for compute: built");
    cp.computeFunction = fs;
    CHECK(pipeline_stage_problem(cp, 2), "a fragment shader for compute: refused");
    cp.computeFunction = k;
    MTLLinkedFunctions *cl = [MTLLinkedFunctions new];
    cl.functions = @[k];
    cp.linkedFunctions = cl;
    CHECK(pipeline_stage_problem(cp, 2), "a kernel as a compute helper: refused");

    MTLTileRenderPipelineDescriptor *tp = [MTLTileRenderPipelineDescriptor new];
    tp.tileFunction = k;
    CHECK(!pipeline_stage_problem(tp, 1), "a kernel for a tile pipeline: built");
    tp.tileFunction = vs;
    CHECK(pipeline_stage_problem(tp, 1), "a vertex shader for a tile pipeline: refused");
    CHECK(!pipeline_stage_problem([NSObject new], 0), "something with no slots: nothing to refuse");
}


/* Direct patch draws (12-15) are bound by their patch index and control point index buffers as the indirect ones
   (18, 19) are: a draw that would read past either is dropped, not handed to the GPU (hv_pvg_patch_ids_problem) */
static void test_patch_bounds(void) {
    printf("patch draws: bound by their index buffers\n");
    CHECK(!hv_pvg_patch_ids_problem(0, 0, 0, 1, 0, 1, 0, 4), "no patches: nothing to read");
    CHECK(!hv_pvg_patch_ids_problem(0, 4, 3, 0, 0, 0, 0, 4), "no index buffers: nothing to bound");
    CHECK(!hv_pvg_patch_ids_problem(2, 2, 3, 1, 16, 0, 0, 4), "patches 2-3 in a 4-id buffer: drawn");
    CHECK(hv_pvg_patch_ids_problem(2, 3, 3, 1, 16, 0, 0, 4), "patches 2-4 in a 4-id buffer: dropped");
    CHECK(hv_pvg_patch_ids_problem(1, 4, 3, 1, 15, 0, 0, 4), "a buffer short of its last id: dropped");
    CHECK(hv_pvg_patch_ids_problem(0xffffffffffffffffull, 2, 3, 1, 16, 0, 0, 4), "a start that wraps: dropped");
    CHECK(!hv_pvg_patch_ids_problem(1, 2, 3, 0, 0, 1, 36, 4), "3 control points a patch, patches 1-2 in 9 ids: drawn");
    CHECK(hv_pvg_patch_ids_problem(1, 3, 3, 0, 0, 1, 36, 4), "patches 1-3 in 9 control point ids: dropped");
    CHECK(hv_pvg_patch_ids_problem(0, 1, 0, 0, 0, 1, 36, 4), "no control points a patch: dropped");
    /* the pipeline's index size: 16-bit control point indexes, a buffer of exactly count x cps x 2 bytes */
    CHECK(!hv_pvg_patch_ids_problem(0, 4, 3, 0, 0, 1, 4 * 3 * 2, 2), "16-bit control point indexes, exactly enough: drawn");
    CHECK(hv_pvg_patch_ids_problem(0, 5, 3, 0, 0, 1, 4 * 3 * 2, 2), "16-bit control point indexes, one patch short: dropped");
    CHECK(hv_pvg_patch_ids_problem(0, 4, 3, 0, 0, 1, 4 * 3 * 2, 4), "the same bytes as 32-bit indexes: dropped");
    CHECK(hv_pvg_patch_ids_problem(0, 4, 3, 0, 0, 1, 4 * 3 * 2, 0), "a pipeline that says neither: counted as 32-bit");
    /* with a patch index buffer the control point indexes are read at the ids in it, not at the places */
    CHECK(!hv_pvg_patch_ids_problem(900, 100, 3, 1, 1000 * 4, 1, 100 * 3 * 4, 4), "patches 900-999 through ids 0-99: drawn");
    CHECK(!hv_pvg_patch_ids_problem(0, 2, 3, 1, 16, 1, 12, 4), "patch ids fit, control points read at the guest's ids: drawn");
    CHECK(hv_pvg_patch_ids_problem(900, 101, 3, 1, 1000 * 4, 1, 100 * 3 * 4, 4), "patch places past their buffer still: dropped");
    /* whatever the ids, each patch reads cps control point indexes: a buffer shorter than that is read past by any id */
    CHECK(hv_pvg_patch_ids_problem(0, 1, 32, 1, 4, 1, 4, 4), "32 control points a patch from one index: dropped");
    CHECK(hv_pvg_patch_ids_problem(0, 1, 3, 1, 4, 1, 4, 2), "3 16-bit indexes a patch from 2: dropped");
    CHECK(!hv_pvg_patch_ids_problem(0, 1, 3, 1, 4, 1, 6, 2), "3 16-bit indexes a patch from exactly 3: drawn");
}

/* The replay's tables of every task swapped with these: a test's own, then back */
static void swap_tasks(HvTaskObjects *__strong *other) {
    for (int t = 0; t < HV_TASKS; t++) {
        HvTaskObjects *x = r->tasks[t];
        r->tasks[t] = other[t], other[t] = x;
    }
}
static void clear_tasks(void) { for (int t = 0; t < HV_TASKS; t++) r->tasks[t] = nil; }

/* A task's table, anew, holding objects at these refs */
static HvTaskObjects *task_with(NSArray<NSNumber *> *refs) {
    HvTaskObjects *t = [HvTaskObjects new];
    for (NSNumber *ref in refs) t[ref] = [HvObject new];
    return t;
}

/* The guest's delete commands, read as Apple's host reads them: the task by its number as it is */
static void test_forget_deleted(void) {
    printf("deletes: a closed task's objects go with it, and only its own\n");
    HvTaskObjects *saved[HV_TASKS];
    swap_tasks(saved);
    r->tasks[3] = task_with(@[@5]);
    r->tasks[6] = task_with(@[@5, @9, @12]);
    uint32_t object[4] = {6, 1005, 12, 9}; /* a function's delete: {task, tag, size, ref} */
    CHECK(forget_deleted(r, 0x28, object, sizeof object), "an object's delete: taken");
    CHECK(!r->tasks[6][@9] && r->tasks[6][@5], "an object's delete: that object gone, its neighbours kept");
    uint32_t resource[2] = {6, 12};
    CHECK(forget_deleted(r, 0x25, resource, sizeof resource), "a resource's delete: taken");
    CHECK(!r->tasks[6][@12] && r->tasks[6][@5], "a resource's delete: that resource gone");
    uint32_t task[1] = {6};
    CHECK(forget_deleted(r, 0x20, task, sizeof task), "a task's delete: taken");
    CHECK(!r->tasks[6], "a task's delete: everything made for it gone (it was kept under 6 when read as 6 >> 1)");
    CHECK(r->tasks[3][@5], "a task's delete: task 3 (6 >> 1) left alone");
    uint32_t other[4] = {3, 0, 0, 0};
    CHECK(!forget_deleted(r, 0x43, other, sizeof other), "another command: not a delete");
    CHECK(forget_deleted(r, 0x20, task, 0) && r->tasks[3][@5], "a task's delete with no task in it: taken, nothing gone");
    /* An object's delete too short to hold its ref: nothing, rather than ref 0 */
    r->tasks[3][@0] = [HvObject new];
    CHECK(forget_deleted(r, 0x28, other, 8) && r->tasks[3][@0], "a short object delete: taken, nothing gone");
    /* What a task asked about its objects goes with it, and the count of all of them with it */
    NSMutableDictionary *saved_answers = r->answers;
    uint32_t saved_count = r->answers_count;
    r->answers = [NSMutableDictionary new];
    r->answers[@3] = [NSMutableDictionary dictionaryWithDictionary:@{[NSData dataWithBytes:"a" length:1]: [NSData data],
                                                                    [NSData dataWithBytes:"b" length:1]: [NSData data]}];
    r->answers[@4] = [NSMutableDictionary dictionaryWithDictionary:@{[NSData dataWithBytes:"c" length:1]: [NSData data]}];
    r->answers_count = 3;
    uint32_t three[1] = {3};
    CHECK(forget_deleted(r, 0x20, three, sizeof three), "a task's delete: taken");
    CHECK(!r->answers[@3] && r->answers[@4].count == 1 && r->answers_count == 1, "its answers gone, the count with them: %u", r->answers_count);
    /* A define gives its number a fresh task (Apple's host refuses one in use): whatever is kept under it goes,
       read as CmdDefineTask2 reads it, the number shifted left one */
    r->tasks[4] = task_with(@[@7]);
    r->tasks[8] = task_with(@[@7]);
    uint32_t define[4] = {4 << 1, 0, 0, 0x1234}; /* {task << 1, size, size high, root page} */
    CHECK(forget_deleted(r, 0x38, define, sizeof define), "a define: taken");
    CHECK(!r->tasks[4] && !r->answers[@4] && r->answers_count == 0, "a define: what was kept under its number gone, answers and count with it: %u", r->answers_count);
    CHECK(r->tasks[8][@7], "a define: task 8 (its first word as it is) left alone");
    r->tasks[4] = task_with(@[@7]);
    CHECK(forget_deleted(r, 0x38, define, 12) && r->tasks[4][@7], "a define too short to be one: taken, nothing gone");
    r->answers = saved_answers, r->answers_count = saved_count;
    clear_tasks();

    /* The way the iPad was inferred to stop: an object kept under a task, the task closed, and the next one given its
       number with the same entry in its table. After the delete the object is made again, not handed over. */
    table[9][0] = 0 | 12 << 8, table[9][1] = 0x900, table[9][2] = 0; /* an empty slot's kind: made without the GPU */
    HvObject *first = [r object:9 task:1];
    CHECK(first && [r object:9 task:1] == first, "the same entry: the object kept");
    uint32_t shifted[1] = {2}; /* the task as it was read before: 1 << 1 */
    forget_deleted(r, 0x20, shifted, sizeof shifted);
    CHECK([r object:9 task:1] == first, "another task's delete: kept");
    uint32_t one[1] = {1};
    forget_deleted(r, 0x20, one, sizeof one);
    HvObject *again = [r object:9 task:1];
    CHECK(again && again != first, "after its task's delete: made again for the same entry");
    memset(table[9], 0, sizeof table[9]);
    swap_tasks(saved);
}

/* A task's delete or define that forgets something is logged at the 1st, 10th, 100th... of each, not once a delete:
   a guest's loop of run and delete would otherwise be a line every two commands */
static void test_forget_logged(void) {
    printf("forgotten tasks: logged at 1, 10, 100...\n");
    HvTaskObjects *saved[HV_TASKS];
    swap_tasks(saved);
    r->tasks_forgot[0] = r->tasks_forgot[1] = 0;
    int fds[2];
    CHECK(pipe(fds) == 0, "a pipe for stderr");
    fflush(stderr);
    int out = dup(2);
    dup2(fds[1], 2);
    for (uint32_t i = 0; i < 150; i++) {
        task_objects(r, 7, YES)[@1] = [HvObject new];
        uint32_t task[1] = {7};
        forget_deleted(r, 0x20, task, sizeof task);
        forget_deleted(r, 0x20, task, sizeof task); /* nothing held: not counted */
    }
    for (uint32_t i = 0; i < 3; i++) {
        task_objects(r, 4, YES)[@1] = [HvObject new];
        uint32_t define[4] = {4 << 1, 0, 0, 0x1234};
        forget_deleted(r, 0x38, define, sizeof define);
    }
    fflush(stderr), dup2(out, 2), close(out), close(fds[1]);
    char got[4096] = {0};
    ssize_t n = read(fds[0], got, sizeof got - 1);
    close(fds[0]);
    BOOL said = strstr(got, "; 100 such so far") != NULL;
    int deleted = 0, defined = 0;
    for (char *at = got; n > 0 && (at = strstr(at, "hengevm: pvg: task ")); at++) {
        char *end = strchr(at, '\n');
        if (end) *end = 0;
        if (strstr(at, "defined again")) defined++;
        else deleted += strstr(at, " deleted:") != NULL;
        if (end) at = end;
    }
    CHECK(r->tasks_forgot[0] == 150 && r->tasks_forgot[1] == 3, "counted: %u deletes, %u defines", r->tasks_forgot[0], r->tasks_forgot[1]);
    CHECK(deleted == 3 && defined == 1, "lines: %d for deletes (1, 10, 100), %d for defines (1)", deleted, defined);
    CHECK(said, "the count said");
    swap_tasks(saved);
}

/* gate M-1: a swap that comes as the guest sends it, an op 8 through hv_pvg_metal_submit, gives its stamp back after
   the presenter's frame that read the old buffer, as finishSwap does (test_swap_waits_for_presenter calls that
   itself). The dispatch must send op 8 there: to finishChannel, it would join the trail from before the frame, and the
   guest could draw into the buffer while the frame still reads it. */
static void test_swap_routed(void) {
    printf("an op 8 from a channel: its stamp after the presenter's frame\n");
    settle();
    unsigned s0 = stamps, told0 = swaps_told;
    int inflight = atomic_load(&r->inflight);
    TestCB *work = exec_ref(6, 2, 46);
    dispatch_sync(r->worker, ^{ [r finishChannel:1 stamp:47]; }); /* a trail behind the guest's work, from before the frame */
    TestCB *trail = q.made.lastObject;
    TestCB *frame = [TestCB new];
    os_unfair_lock_lock(&r->screen_lock);
    presenter_take(r, (id<MTLCommandBuffer>)frame); /* the screen's texture taken; the frame still being encoded */
    os_unfair_lock_unlock(&r->screen_lock);
    NSUInteger made = q.made.count;
    uint32_t *swap = calloc(1, 52); /* {op 8, length, stamp} then the swap's payload: port 0, mapping 5, no gamma */
    swap[0] = 8, swap[1] = 52, swap[2] = 48, swap[5] = 5;
    hv_pvg_metal_submit((__bridge HvPvgMetal *)r, NULL, 1, swap, 52);
    settle();
    CHECK(swaps_told == told0 + 1, "the swap reached the display (%u)", swaps_told - told0);
    [work finish:YES], [trail finish:YES];
    settle();
    CHECK(stamps == s0 + 2, "the work's and the earlier stamp, not the swap's while the frame is in flight (%u)", stamps - s0);
    [frame commit], [frame finish:YES];
    for (NSUInteger i = made; i < q.made.count; i++) [q.made[i] finish:YES];
    settle();
    CHECK(stamps == s0 + 3 && atomic_load(&r->inflight) == inflight, "the swap's once the frame is done (%u), nothing left in flight",
          stamps - s0);
}

/* Every command handed to the worker, from a channel (hv_pvg_metal_submit) or the root ring (hv_pvg_metal_forget), goes
   to the worker's copy of the task tables, on the worker, in order, with its barriers left out: a define or an object
   list is what the reads after it use (pvg_lite.c's worker_tasks) */
static void test_task_commands(void) {
    printf("task commands: to the worker's tables, in order\n");
    ntold_task = 0;
    uint32_t def[9] = {0x10038, 36, 7, 2, 3, 5 << 1, 0x4000, 0, 0x77}; /* one barrier, then {task << 1, size, 0, root} */
    uint32_t list[10] = {0x44, 40, 8, 5, 0x1000, 0, 0x2000, 0, 0x40, 0x40}; /* {task, objects, placement, their lengths} */
    uint32_t *a = malloc(sizeof def), *b = malloc(sizeof list), *c = malloc(sizeof def);
    memcpy(a, def, sizeof def), memcpy(b, list, sizeof list), memcpy(c, def, sizeof def);
    c[0] = 0x38, c[1] = 28, c[2] = 9, memmove(c + 3, def + 5, 16); /* the same define, no barrier, on the root ring */
    unsigned before = stamps;
    hv_pvg_metal_submit((__bridge HvPvgMetal *)r, NULL, 1, a, sizeof def);
    hv_pvg_metal_submit((__bridge HvPvgMetal *)r, NULL, 1, b, sizeof list);
    hv_pvg_metal_forget((__bridge HvPvgMetal *)r, c, 28);
    dispatch_sync(r->worker, ^{});
    CHECK(ntold_task == 3, "three told: %u", ntold_task);
    CHECK(told_task[0][0] == 0x38 && told_task[0][1] == 16 && told_task[0][2] == 5 << 1, "the channel's define, past its barrier: %x %u %x",
          told_task[0][0], told_task[0][1], told_task[0][2]);
    CHECK(told_task[1][0] == 0x44 && told_task[1][1] == 28 && told_task[1][2] == 5, "then its object list: %x %u %x", told_task[1][0],
          told_task[1][1], told_task[1][2]);
    CHECK(told_task[2][0] == 0x38 && told_task[2][1] == 16 && told_task[2][2] == 5 << 1, "then the root ring's define: %x %u %x",
          told_task[2][0], told_task[2][1], told_task[2][2]);
    CHECK(told_task[0][3] && told_task[1][3] && told_task[2][3], "all on the worker");
    CHECK(stamps == before + 2, "the channel's two finished: %u", stamps - before);
}

/* A surface the guest unmaps takes what was made over it out of the cache: none of it is used again, and kept it held
   the surface's pages and GPU mapping until each ref was named again (hv_pvg_metal_unmap, over_surface) */
static void test_unmap_lets_go(void) {
    printf("unmap: objects over the surface leave the cache\n");
    HvTaskObjects *saved[HV_TASKS];
    swap_tasks(saved);
    HvObject *s = [HvObject new], *tex = [HvObject new], *view = [HvObject new], *other = [HvObject new], *newer = [HvObject new];
    tex->surface = s, tex->cache_task = 2, tex->cache_ref = 10;
    over_surface(tex);
    view->surface = s, view->parent = tex, view->cache_task = 3, view->cache_ref = 4;
    over_surface(view);
    HvObject *moved = [HvObject new]; /* over it once, its ref since given to another object */
    moved->surface = s, moved->cache_task = 2, moved->cache_ref = 11;
    over_surface(moved);
    r->tasks[2] = [HvTaskObjects new], r->tasks[3] = [HvTaskObjects new];
    r->tasks[2][@10] = tex, r->tasks[2][@11] = newer, r->tasks[2][@12] = other, r->tasks[3][@4] = view;
    r->surfaces[@777] = s;
    CHECK(s->over.count == 3, "the surface knows what is over it: %lu", (unsigned long)s->over.count);
    hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 777);
    dispatch_sync(r->worker, ^{});
    CHECK(!r->tasks[2][@10] && !r->tasks[3][@4], "the texture over it and its view: out of the cache");
    CHECK(r->tasks[2][@11] == newer && r->tasks[2][@12] == other, "a ref given to another object since, and others: kept");
    CHECK(s->gone && !s->over && !r->surfaces[@777], "the surface: gone, and keeps no list");
    set_unmapped(r, 777, NO);
    swap_tasks(saved);
}

/* (gate r1 should 5) A frame taken (presenter_pick's DRAW) is committed whatever happens before its end: a target
   change between the take and the commit (set_layer) leaves it unpresented but committed, for enqueued at the take it
   holds up every later GPU buffer (the guest's drawing, a swap's stamp) until it is. And the screen is drawn again for
   the new target, the Mac's waiting picture kept waiting. */
static void test_presenter_commits(void) {
    printf("the presenter: a frame taken is always committed, a target change in between or not\n");
    id fake_layer = [NSObject new], other_layer = [NSObject new], drawable = [NSObject new];
    os_unfair_lock_lock(&r->screen_lock);
    CAMetalLayer *saved_layer = r->layer;
    uint64_t saved_waiting = r->swap_waiting_ns;
    r->layer = fake_layer, r->screen_dirty = NO, r->swap_waiting_ns = 0;
    uint64_t generation = r->layer_generation, frames = r->gpu_frames, holds = atomic_load(&r->present_holds);
    os_unfair_lock_unlock(&r->screen_lock);
    /* The same target: presented and committed */
    TestCB *cb = [TestCB new];
    [cb enqueue]; /* the take */
    BOOL shown = presenter_commit(r, generation, fake_layer, (id<MTLCommandBuffer>)cb, (id<CAMetalDrawable>)drawable, 0, stage_now());
    CHECK(shown && cb.presented == drawable && cb.status == MTLCommandBufferStatusCommitted && r->gpu_frames == frames + 1,
          "the same target: presented and committed (status %d)", (int)cb.status);
    /* The target changed after the take (set_layer, under the same lock) */
    os_unfair_lock_lock(&r->screen_lock);
    r->layer = other_layer, r->layer_generation++;
    os_unfair_lock_unlock(&r->screen_lock);
    TestCB *late = [TestCB new];
    [late enqueue];
    shown = presenter_commit(r, generation, fake_layer, (id<MTLCommandBuffer>)late, (id<CAMetalDrawable>)drawable, 4321, stage_now());
    os_unfair_lock_lock(&r->screen_lock);
    BOOL dirty = r->screen_dirty;
    uint64_t waiting = r->swap_waiting_ns;
    r->layer = saved_layer, r->layer_generation = generation, r->screen_dirty = NO, r->swap_waiting_ns = saved_waiting;
    os_unfair_lock_unlock(&r->screen_lock);
    CHECK(!shown && !late.presented && r->gpu_frames == frames + 1, "a target change since the take: not presented to the old target");
    CHECK(late.status == MTLCommandBufferStatusCommitted, "but committed all the same: the GPU work behind it not held up (status %d)",
          (int)late.status);
    CHECK(dirty && waiting == 4321, "drawn again for the new target, the Mac's picture still waiting (%llu)", waiting);
    CHECK(atomic_load(&r->present_holds) == holds + 2, "both holds counted");
}

/* 5c: unmapping the surface on screen lets the screen go of it (its texture is over pages the guest gave back), a late
   CPU present of it does not happen, an unknown id is not put on screen, and the presenter given no screen after its
   wait takes nothing and stops */
static void test_unmap_on_screen(void) {
    printf("unmap: the surface on screen is let go of, and the presenter stops\n");
    HvObject *s = [HvObject new], *other = [HvObject new];
    id sentinel = [NSObject new], fake_layer = [NSObject new];
    __weak id weak_sentinel = sentinel;
    r->surfaces[@777] = s, r->surfaces[@778] = other;
    os_unfair_lock_lock(&r->screen_lock);
    r->screen_tex = sentinel, r->screen_w = 64, r->screen_h = 48;
    os_unfair_lock_unlock(&r->screen_lock);
    sentinel = nil;
    r->on_screen = 777, r->late = YES, r->late_mapping = 777;
    hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 778); /* not the one shown */
    dispatch_sync(r->worker, ^{});
    CHECK(r->screen_tex && r->on_screen == 777 && r->late, "another surface unmapped: the screen keeps its own");
    hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 777);
    dispatch_sync(r->worker, ^{});
    CHECK(!r->screen_tex && !weak_sentinel, "the surface shown, unmapped: the screen's texture let go of");
    CHECK(r->on_screen == 0, "and nothing on screen: %llu", r->on_screen);
    CHECK(!r->late, "a late CPU present of it: dropped");
    dispatch_sync(r->worker, ^{ [r present:777]; });
    CHECK(r->on_screen == 0, "a present of the unmapped id does not bring it back: %llu", r->on_screen);
    /* After nextDrawable, under the lock: what to draw */
    os_unfair_lock_lock(&r->screen_lock);
    CAMetalLayer *saved_layer = r->layer;
    r->layer = fake_layer, r->presenter_running = YES, r->screen_dirty = NO;
    uint64_t generation = r->layer_generation, presents = atomic_load(&r->presents);
    int inflight = atomic_load(&r->inflight);
    uint64_t swap = 0, swap_saved = r->swap_waiting_ns;
    r->swap_waiting_ns = 1234;
    PresenterPick pick = presenter_pick(r, generation, fake_layer, 64, 48, YES, NO, nil, &swap);
    BOOL running = r->presenter_running;
    os_unfair_lock_unlock(&r->screen_lock);
    CHECK(pick == PICK_STOP && !running, "no screen after the wait: stop, the presenter no longer running (pick %d)", pick);
    CHECK(atomic_load(&r->presents) == presents && atomic_load(&r->inflight) == inflight, "and nothing taken");
    os_unfair_lock_lock(&r->screen_lock);
    r->screen_tex = (id)[NSObject new], r->presenter_running = YES;
    r->screen_half = NO;
    pick = presenter_pick(r, generation, fake_layer, 64, 32, YES, NO, nil, &swap);
    BOOL dirty = r->screen_dirty;
    r->screen_dirty = NO;
    PresenterPick reformatted = presenter_pick(r, generation, fake_layer, 64, 48, YES, YES, nil, &swap); /* made for half floats */
    BOOL reformatted_dirty = r->screen_dirty;
    PresenterPick moved = presenter_pick(r, generation + 1, fake_layer, 64, 48, YES, NO, nil, &swap);
    uint64_t waiting = r->swap_waiting_ns;
    uint64_t presents_again = atomic_load(&r->presents);
    /* And a draw: the frame is counted, and takes its place in the GPU's order, in the same lock hold that read the
       screen's texture (item 1), so a swap's stamp from now on waits for it. The real presentLoop's order: none of
       test_swap_waits_for_presenter's hand-made takes stands for it. */
    r->swap_waiting_ns = 5678, r->screen_dirty = YES;
    TestCB *frame = [TestCB new];
    uint64_t drawn_swap = 0;
    PresenterPick drew = presenter_pick(r, generation, fake_layer, 64, 48, YES, NO, (id<MTLCommandBuffer>)frame, &drawn_swap);
    BOOL drew_dirty = r->screen_dirty;
    uint64_t drew_waiting = r->swap_waiting_ns;
    BOOL still_running = r->presenter_running;
    r->swap_waiting_ns = swap_saved;
    r->screen_tex = nil, r->layer = saved_layer, r->presenter_running = NO, r->screen_dirty = NO;
    os_unfair_lock_unlock(&r->screen_lock);
    CHECK(pick == PICK_AGAIN && dirty && moved == PICK_AGAIN, "a mode or target change in the wait: round again");
    CHECK(reformatted == PICK_AGAIN && reformatted_dirty,
          "the screen's format changed since what was made before the pick: round again, nothing made in the hold (pick %d)", reformatted);
    CHECK(presents_again == presents, "still nothing taken");
    CHECK(swap == 0 && waiting == 1234, "the Mac's waiting picture is not taken by a stop or a round again: %llu %llu", swap, waiting);
    CHECK(drew == PICK_DRAW && still_running, "the screen there, the same target and size: draw (pick %d)", drew);
    CHECK(atomic_load(&r->presents) == presents + 1 && atomic_load(&r->inflight) == inflight + 1,
          "the frame counted at the pick: presents %llu, inflight %d", atomic_load(&r->presents) - presents, atomic_load(&r->inflight) - inflight);
    CHECK(frame.status == MTLCommandBufferStatusEnqueued, "and its place on the GPU taken with the texture");
    CHECK(!drew_dirty, "the screen no longer waiting to be drawn");
    CHECK(drawn_swap == 5678 && drew_waiting == 0, "the Mac's waiting picture: the frame's (%llu, %llu left)", drawn_swap, drew_waiting);
    unsigned s0 = stamps;
    NSUInteger made = q.made.count;
    dispatch_sync(r->worker, ^{ [r finishSwap:1 stamp:44]; });
    settle();
    CHECK(stamps == s0, "a swap's stamp after the pick: not while the frame is in flight");
    [frame commit], [frame finish:YES];
    for (NSUInteger i = made; i < q.made.count; i++) [q.made[i] finish:YES];
    settle();
    CHECK(stamps == s0 + 1 && atomic_load(&r->inflight) == inflight, "given once the frame is done (%u), nothing left in flight", stamps - s0);
    set_unmapped(r, 777, NO), set_unmapped(r, 778, NO);
}

/* A map of an id that still has a surface: its unmap was passed over (a ring restart, or a jump of the mapper's index),
   so the surface is over the pages of the id's last mapping. It is dropped, as an unmap drops it, and the copy-backs
   still to come through it are not made unless another mapping has the same video (the same guest pages). */
static void test_stale_map(void) {
    printf("map: a surface whose unmap was passed over is dropped\n");
    if (!r->planar_kept) r->planar_kept = [NSMutableArray new];
    HvVideo *v = [HvVideo new]; /* no IOSurface: none of this reaches one */
    v->video_lock = OS_UNFAIR_LOCK_INIT;
    HvObject *s7 = [HvObject new], *s8 = [HvObject new], *plain = [HvObject new];
    s7->video = v, s8->video = v;
    r->surfaces[@7] = s7, r->surfaces[@8] = s8, r->surfaces[@9] = plain;
    id sentinel = [NSObject new];
    __weak id weak_sentinel = sentinel;
    os_unfair_lock_lock(&r->screen_lock);
    r->screen_tex = sentinel;
    os_unfair_lock_unlock(&r->screen_lock);
    sentinel = nil;
    r->on_screen = 9, r->late = YES, r->late_mapping = 9;
    uint64_t dropped = r->stale_dropped;
    hv_pvg_metal_map((__bridge HvPvgMetal *)r, 10); /* nothing under it: nothing dropped */
    dispatch_sync(r->worker, ^{});
    CHECK(r->stale_dropped == dropped && r->surfaces.count >= 3, "a map of an id with no surface: nothing dropped");
    hv_pvg_metal_map((__bridge HvPvgMetal *)r, 9);
    dispatch_sync(r->worker, ^{});
    CHECK(plain->gone && !r->surfaces[@9] && r->stale_dropped == dropped + 1, "mapped again with no unmap: the old one dropped");
    CHECK(!r->screen_tex && !weak_sentinel && r->on_screen == 0 && !r->late, "it was on screen: the screen lets go of it, as an unmap does");
    CHECK(!is_unmapped(9), "and the id is mapped");
    hv_pvg_metal_map((__bridge HvPvgMetal *)r, 7);
    dispatch_sync(r->worker, ^{});
    CHECK(s7->gone && !r->surfaces[@7] && r->surfaces[@8] == s8, "one of two mappings over a video dropped: the other kept");
    CHECK(!atomic_load(&s7->backs_off), "its copy-backs still made: the other mapping has the same pages");
    hv_pvg_metal_map((__bridge HvPvgMetal *)r, 8);
    dispatch_sync(r->worker, ^{});
    CHECK(s8->gone && !r->surfaces[@8] && atomic_load(&s8->backs_off), "the last mapping over it dropped: its copy-backs are not made");
    v->back_gen = 3;
    uint64_t backs = r->video_backs;
    video_back(r, s8, 9);
    CHECK(r->video_backs == backs && v->back_gen == 9, "a copy-back through it: not made, and no longer to come (gen %llu)", v->back_gen);
    hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 7);
    hv_pvg_metal_unmap((__bridge HvPvgMetal *)r, 7);
    dispatch_sync(r->worker, ^{});
    CHECK(r->stale_dropped == dropped + 3 && is_unmapped(7), "an unmap is no stale drop; the id held unmapped");
    set_unmapped(r, 7, NO);
    [r->planar_kept removeAllObjects], r->planar_kept_bytes = 0;
}

/* A pipeline description Metal's own checks would stop the app on is refused first: read from Metal's descriptor
   objects (pipe_desc), checked as this GPU takes them (hv_pvg_pipeline_problem) */
/* The pipeline being built is said if the process dies building it (note_building, hv_pvg_metal_crash_note): each
   shader with its kind, the guest's ref for it and its library's hash; only on the thread building, only until built */
static void *note_elsewhere(void *unused) { (void)unused; hv_pvg_metal_crash_note(); return NULL; }
static void test_building_note(void) {
    printf("the pipeline being built, said at a crash\n");
    id vs = test_function(MTLFunctionTypeVertex, @"vs"), fs = test_function(MTLFunctionTypeFragment, @"wrong_kind");
    objc_setAssociatedObject(vs, &lib_hash_key, @(0x1234abcdull), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    NSMapTable *refs = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsObjectPointerPersonality valueOptions:NSPointerFunctionsStrongMemory];
    [refs setObject:@77 forKey:vs];
    MTLRenderPipelineDescriptor *rp = [MTLRenderPipelineDescriptor new];
    rp.vertexFunction = vs, rp.fragmentFunction = fs;
    note_building(rp, 0, 5, 1234, refs);
    CHECK(strstr(building_text, "render pipeline (task 5, ref 1234)") != NULL, "names the task and the pipeline's ref");
    CHECK(strstr(building_text, "vertex 'vs' (a vertex function, ref 77, library 000000001234abcd)") != NULL, "the vertex slot: name, kind, ref, library");
    CHECK(strstr(building_text, "fragment 'wrong_kind' (a fragment function, ref -1,") != NULL, "a function with no ref known: -1");
    CHECK(building_text[strlen(building_text) - 1] == '\n', "one line");
    int fds[2];
    CHECK(pipe(fds) == 0, "a pipe for stderr");
    int saved = dup(2);
    dup2(fds[1], 2);
    hv_pvg_metal_crash_note();
    pthread_t other; /* another thread: not building (a dispatch_sync could run on this one) */
    pthread_create(&other, NULL, note_elsewhere, NULL);
    pthread_join(other, NULL);
    built();
    hv_pvg_metal_crash_note(); /* built: nothing */
    dup2(saved, 2), close(saved), close(fds[1]);
    char got[4096] = {0};
    ssize_t n = read(fds[0], got, sizeof got - 1);
    close(fds[0]);
    CHECK(n == (ssize_t)strlen(building_text) && !memcmp(got, building_text, (size_t)n), "said once: by the building thread, while building");
    MTLComputePipelineDescriptor *cp = [MTLComputePipelineDescriptor new];
    note_building(cp, 2, 3, 9, nil);
    built();
    CHECK(strstr(building_text, "compute pipeline (task 3, ref 9) when the process died: compute none; 0 helpers") != NULL, "a compute pipeline with no kernel");
    char longname[2000];
    memset(longname, 'x', sizeof longname - 1), longname[sizeof longname - 1] = 0;
    rp.vertexFunction = (id)test_function(MTLFunctionTypeVertex, [NSString stringWithUTF8String:longname]);
    note_building(rp, 0, 1, 1, nil);
    built();
    size_t len = strlen(building_text);
    CHECK(len < sizeof building_text && building_text[len - 1] == '\n', "a name longer than the note: cut, still one line");
}

static void test_pipeline_checks(void) {
    printf("pipeline descriptions Metal would stop the app on\n");
    HvTexCaps caps = {.bc = 0, .samples = 1u << 1 | 1u << 4, .color_bytes = 128, .implicit_bytes = 256 << 10};
    id vs = test_function(MTLFunctionTypeVertex, @"vs"), fs = test_function(MTLFunctionTypeFragment, @"fs");
    id k = test_function(MTLFunctionTypeKernel, @"k");
    MTLRenderPipelineDescriptor *rp = [MTLRenderPipelineDescriptor new];
    HvPipe p = pipe_desc(rp, 0);
    CHECK(hv_pvg_pipeline_problem(&p, caps), "no vertex function (Metal: must not be nil): refused");
    rp.vertexFunction = vs, rp.fragmentFunction = fs;
    rp.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    rp.colorAttachments[0].blendingEnabled = YES;
    rp.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    p = pipe_desc(rp, 0);
    CHECK(!hv_pvg_pipeline_problem(&p, caps), "BGRA8 blended, Depth32Float: built");
    rp.fragmentFunction = nil;
    p = pipe_desc(rp, 0);
    CHECK(!hv_pvg_pipeline_problem(&p, caps), "no fragment function: built");
    rp.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA16Uint;
    rp.colorAttachments[1].blendingEnabled = YES;
    p = pipe_desc(rp, 0);
    CHECK(p.color[1] == MTLPixelFormatRGBA16Uint && p.blending[1] && hv_pvg_pipeline_problem(&p, caps), "blending an integer format: refused");
    rp.colorAttachments[1].blendingEnabled = NO;
    p = pipe_desc(rp, 0);
    CHECK(!hv_pvg_pipeline_problem(&p, caps), "the integer format unblended: built");
    rp.colorAttachments[1].pixelFormat = MTLPixelFormatInvalid;
    rp.alphaToCoverageEnabled = YES;
    rp.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
    rp.colorAttachments[0].blendingEnabled = NO;
    p = pipe_desc(rp, 0);
    CHECK(hv_pvg_pipeline_problem(&p, caps), "alpha to coverage on an integer first target: refused");
    rp.alphaToCoverageEnabled = NO;
    rp.colorAttachments[0].pixelFormat = MTLPixelFormatBC1_RGBA;
    p = pipe_desc(rp, 0);
    CHECK(hv_pvg_pipeline_problem(&p, caps), "a compressed colour target: refused");
    rp.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    rp.depthAttachmentPixelFormat = MTLPixelFormatStencil8;
    p = pipe_desc(rp, 0);
    CHECK(hv_pvg_pipeline_problem(&p, caps), "Stencil8 as the depth format: refused");
    rp.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    rp.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    p = pipe_desc(rp, 0);
    CHECK(!hv_pvg_pipeline_problem(&p, caps), "the combined format for both: built");
    rp.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
    p = pipe_desc(rp, 0);
    CHECK(hv_pvg_pipeline_problem(&p, caps), "combined depth with another stencil (Metal: must match): refused");
    p.stencil = 255; /* Depth24Unorm_Stencil8: a Mac's, not this GPU's */
    p.depth = 0;
    CHECK(hv_pvg_pipeline_problem(&p, caps), "a Mac-only stencil format: refused");
    rp.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float, rp.stencilAttachmentPixelFormat = MTLPixelFormatStencil8;
    p = pipe_desc(rp, 0);
    CHECK(!hv_pvg_pipeline_problem(&p, caps), "separate depth and stencil formats: built");
    for (int i = 0; i < 5; i++) rp.colorAttachments[i].pixelFormat = MTLPixelFormatRGBA32Float;
    p = pipe_desc(rp, 0);
    CHECK(!hv_pvg_pipeline_problem(&p, caps), "5 x 16 bytes a sample, within an Apple7 GPU's 128: built");
    caps.color_bytes = 64;
    CHECK(hv_pvg_pipeline_problem(&p, caps), "past an Apple4-6 GPU's 64: refused");
    caps.color_bytes = 0;
    CHECK(!hv_pvg_pipeline_problem(&p, caps), "on a GPU without tile memory: built");
    caps.color_bytes = 128;
    /* Tessellation, topology, sample count and vertex buffers, as Metal words them; Metal's defaults pass */
    MTLRenderPipelineDescriptor *t = [MTLRenderPipelineDescriptor new];
    t.vertexFunction = vs;
    HvPipe q = pipe_desc(t, 0);
    CHECK(!hv_pvg_pipeline_problem(&q, caps) && q.tess_factor == 16 && q.raster_samples == 1, "Metal's defaults: built (factor %u)", q.tess_factor);
    t.maxTessellationFactor = 64;
    q = pipe_desc(t, 0);
    CHECK(!hv_pvg_pipeline_problem(&q, caps), "factor 64, power of 2: built");
    q.tess_factor = 65;
    CHECK(hv_pvg_pipeline_problem(&q, caps), "factor 65: refused");
    q.tess_factor = 0;
    CHECK(hv_pvg_pipeline_problem(&q, caps), "factor 0: refused");
    q.tess_factor = 12;
    CHECK(hv_pvg_pipeline_problem(&q, caps), "factor 12 with power-of-2 partitions: refused");
    q.tess_mode = MTLTessellationPartitionModeInteger;
    CHECK(!hv_pvg_pipeline_problem(&q, caps), "factor 12 with integer partitions: built");
    q.tess_mode = MTLTessellationPartitionModeFractionalOdd, q.tess_factor = 13;
    CHECK(hv_pvg_pipeline_problem(&q, caps), "an odd factor with fractional partitions: refused");
    q.tess_factor = 14;
    CHECK(!hv_pvg_pipeline_problem(&q, caps), "an even one: built");
    q.tess_mode = 4;
    CHECK(hv_pvg_pipeline_problem(&q, caps), "a partition mode Metal doesn't have: refused");
    q = pipe_desc(t, 0), q.topology = 4;
    CHECK(hv_pvg_pipeline_problem(&q, caps), "a topology Metal doesn't have: refused");
    t.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    t.rasterSampleCount = 4;
    t.vertexDescriptor.attributes[3].format = MTLVertexFormatFloat4;
    t.vertexDescriptor.attributes[3].bufferIndex = 30;
    t.vertexDescriptor.layouts[30].stride = 16;
    q = pipe_desc(t, 0);
    CHECK(q.vertex_buffer == 31 && !hv_pvg_pipeline_problem(&q, caps), "4 samples, triangles, an attribute from buffer 30: built (%llu)", (unsigned long long)q.vertex_buffer);
    q.vertex_buffer = 32;
    CHECK(hv_pvg_pipeline_problem(&q, caps), "an attribute from buffer 31: refused");
    q = pipe_desc(t, 0), q.raster_samples = 8;
    CHECK(hv_pvg_pipeline_problem(&q, caps), "8 samples on a GPU that takes 1 and 4: refused");
    q.raster_samples = 40;
    CHECK(hv_pvg_pipeline_problem(&q, caps), "40 samples: refused");
    MTLTileRenderPipelineDescriptor *tile = [MTLTileRenderPipelineDescriptor new];
    tile.tileFunction = k, tile.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm, tile.rasterSampleCount = 2;
    q = pipe_desc(tile, 1);
    CHECK(q.raster_samples == 2 && hv_pvg_pipeline_problem(&q, caps), "a tile pipeline of 2 samples here: refused");
    MTLTileRenderPipelineDescriptor *tp = [MTLTileRenderPipelineDescriptor new];
    tp.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    p = pipe_desc(tp, 1);
    CHECK(hv_pvg_pipeline_problem(&p, caps), "a tile pipeline without its function: refused");
    tp.tileFunction = k;
    p = pipe_desc(tp, 1);
    CHECK(!hv_pvg_pipeline_problem(&p, caps), "with it: built");
    MTLComputePipelineDescriptor *cp = [MTLComputePipelineDescriptor new];
    p = pipe_desc(cp, 2);
    CHECK(hv_pvg_pipeline_problem(&p, caps), "a compute pipeline without its function: refused");
    cp.computeFunction = k;
    p = pipe_desc(cp, 2);
    CHECK(!hv_pvg_pipeline_problem(&p, caps), "with it: built");
}

/* Views of views, as long a chain as the guest likes, made on a thread with a worker's stack: one deep each, not one
   per view in the chain. And a chain that comes back on itself. */
static void test_view_chains(void) {
    printf("texture views of views: a long chain, and a loop\n");
    HvTaskObjects *saved[HV_TASKS];
    swap_tasks(saved);
    for (uint32_t i = 2; i < VIEWS; i++) views_parent[i] = i - 1; /* down to ref 1, an empty slot */
    dispatch_queue_t worker = dispatch_queue_create("test.views", DISPATCH_QUEUE_SERIAL);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block HvObject *last;
    dispatch_async(worker, ^{ last = [r object:VIEWS - 1 task:2]; dispatch_semaphore_signal(done); });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    CHECK(last && !last->object && r->tasks[2]->count == VIEWS - 1, "%u views, of nothing in the end: each made once, %lu kept",
          VIEWS - 2, (unsigned long)r->tasks[2]->count);
    /* 10 -> 11 -> ... -> 5000 -> 10, reached from 9 */
    clear_tasks();
    NSMutableSet *told = r->complaints;
    BOOL full = r->complaints_full;
    r->complaints = [NSMutableSet new], r->complaints_full = NO; /* the chain's "view of nothing"s filled it */
    for (uint32_t i = 10; i < 5000; i++) views_parent[i] = i + 1;
    views_parent[5000] = 10, views_parent[9] = 10;
    dispatch_async(worker, ^{ last = [r object:9 task:2]; dispatch_semaphore_signal(done); });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    CHECK(last && !last->object && r->tasks[2][@5000] && !((HvObject *)r->tasks[2][@5000])->object &&
          [r->complaints containsObject:@"texture views in a loop: views of nothing"], "a loop: views of nothing, told");
    dispatch_async(worker, ^{ last = [r object:5000 task:2]; dispatch_semaphore_signal(done); });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    CHECK(last == r->tasks[2][@5000], "and kept so while their entries stay");
    r->complaints = told, r->complaints_full = full;
    swap_tasks(saved);
}

/* A function's record names nothing but its library, and Apple's host takes only tag 0xf there */
/* gfx-cost 7: the debugging switches are read once, at create; none is read again by an exec, a function or a pipeline
   (one was read for each resource of each exec, about 100 ns each) */
static void test_env_once(void) {
    printf("an exec reads no environment switch\n");
    object(9);
    unsigned before = getenv_calls;
    for (uint32_t i = 0; i < 100; i++) { /* each one finished, so the in-flight cap never holds the next */
        [exec_ref(9, i & 1, 200 + i) finish:YES];
        settle();
    }
    [r makeFunction:0x8000 task:1];
    settle();
    CHECK(getenv_calls == before, "100 execs and a function: %u reads (none)", getenv_calls - before);
    [r->tasks[1] removeObjectForKey:@9];
}

/* An object that counts its going */
static unsigned counted_gone;
@interface Counted : HvObject
@end
@implementation Counted
- (void)dealloc { counted_gone++; }
@end

/* gfx-cost 7: an exec's resource is looked up once, whatever its flags say to do with it after; a task's table is
   by ref, as big as the objects in it */
static void test_lookup_once(void) {
    printf("an exec's resources looked up once; a task's table by ref\n");
    object(9);
    unsigned before = task_reads;
    [exec_ref(9, 2 | 4, 300) finish:YES]; /* drawn into, and wanted back after */
    settle();
    CHECK(task_reads == before + 1, "a resource drawn into and wanted back: its entry read once (%u)", task_reads - before);
    [r->tasks[1] removeObjectForKey:@9];

    HvTaskObjects *t = [HvTaskObjects new];
    counted_gone = 0;
    @autoreleasepool { /* what objs_get hands back is let go at its end */
    HvObject *a = [Counted new], *b = [Counted new], *c = [Counted new];
    CHECK(objs_set(t, 3, a) && objs_set(t, 3000000, b) && objs_set(t, 1025, c), "set near and far");
    CHECK(t->cap == HV_OBJS_MIN && t->count == 3, "the table as big as the objects in it, not the refs: %u slots", t->cap);
    CHECK(objs_get(t, 3) == a && objs_get(t, 3000000) == b && objs_get(t, 1025) == c && !objs_get(t, 4) && !objs_get(t, 4000000000u),
          "each found at its ref, none elsewhere");
    uint32_t at = 0, ref = 0;
    HvObject *seen[4] = {nil};
    int n = 0, found = 0;
    for (HvObject *o; n < 4 && (o = objs_next(t, &at, &ref)); at++, n++)
        seen[n] = o, found += (o == a && ref == 3) + (o == b && ref == 3000000) + (o == c && ref == 1025);
    CHECK(n == 3 && found == 3, "all of them walked, each with its ref");
    objs_set(t, 1025, nil), objs_set(t, 7, nil);
    CHECK(!objs_get(t, 1025) && t->count == 2, "one taken out; one never there taken out: nothing (%lu)", (unsigned long)t->count);
    objs_set(t, 0xffffffffu, [Counted new]);
    CHECK(objs_get(t, 0xffffffffu) && objs_get(t, 3) == a, "the last ref there can be");
    for (int i = 0; i < 4; i++) seen[i] = nil;
    a = b = c = nil;
    }
    CHECK(counted_gone == 1, "one taken out: let go (%u)", counted_gone);
    /* (review) refs 1024 apart, as a guest could name them: 16 bytes a slot, at most twice the objects */
    HvTaskObjects *far = [HvTaskObjects new];
    HvObject *one = [HvObject new];
    for (uint32_t i = 0; i < 20000; i++) objs_set(far, i * 1024 + 5, one);
    CHECK(far->count == 20000 && far->cap <= 65536, "20,000 objects 1024 refs apart: %u slots, %lu KB", far->cap,
          (unsigned long)far->cap * 16 / 1024);
    BOOL all = YES;
    for (uint32_t i = 0; i < 20000; i++) all &= objs_get(far, i * 1024 + 5) == one && !objs_get(far, i * 1024 + 6);
    CHECK(all, "each found, and its neighbours not");
    for (uint32_t i = 0; i < 20000; i += 2) objs_set(far, i * 1024 + 5, nil); /* every other one out: what follows moves */
    all = far->count == 10000;
    for (uint32_t i = 0; i < 20000; i++) all &= (objs_get(far, i * 1024 + 5) == one) == (i & 1);
    CHECK(all, "half taken out: the others all still found (%lu)", (unsigned long)far->count);
    for (uint32_t i = 1; i < 20000; i += 2) objs_set(far, i * 1024 + 5, nil);
    CHECK(far->count == 0 && far->cap == HV_OBJS_MIN, "all out: back to its smallest (%u slots)", far->cap);
    CHECK(counted_gone == 1, "one taken out: let go (%u)", counted_gone);
    @autoreleasepool { t = nil; }
    CHECK(counted_gone == 4, "the table let go: its objects with it (%u)", counted_gone);
}

/* (review 5) An exec a throw cuts short (caught in hv_pvg_metal_submit; ARC releases nothing on one): the objects it
   had looked up are let go by the next exec, not held for good with their memory and GPU mappings */
static void test_exec_throw(void) {
    printf("an exec cut short by a throw: its objects let go by the next\n");
    id saved_dev = r->device;
    uint32_t saved_rec[7];
    memcpy(saved_rec, sampler_record, sizeof saved_rec);
    counted_gone = 0;
    @autoreleasepool {
        table[13][0] = 0x77 | 12 << 8, table[13][1] = 13;
        HvObject *kept = [Counted new];
        memcpy(kept->entry, table[13], 12);
        task_objects(r, 1, YES)[@13] = kept;
        HvObject *heap = object(11);
        heap->object = [NSObject new], heap->kind = KIND_HEAP; /* asked to place a buffer: throws */
        table[12][0] = 14 | 20 << 8, table[12][1] = 0x9000, table[12][2] = 0;
        struct __attribute__((packed)) { uint32_t heap; uint64_t offset, length; } at = {11, 0x1000, 0x1000};
        memset(sampler_record, 0, sizeof sampler_record), memcpy(sampler_record, &at, sizeof at);
        r->device = (id)[HeapDevice new];
        uint32_t e[7] = {1, 2, 0, 13, 0, 12, 0};
        BOOL threw = NO;
        @try { [r exec:e length:sizeof e channel:1 stamp:400]; } @catch (NSException *x) { (void)x; threw = YES; }
        CHECK(threw, "the exec threw, partway through its resources");
        [task_objects(r, 1, YES) removeObjectForKey:@13];
        [task_objects(r, 1, YES) removeObjectForKey:@12];
        [task_objects(r, 1, YES) removeObjectForKey:@11];
        kept = nil, heap = nil;
    }
    r->device = saved_dev;
    memcpy(sampler_record, saved_rec, sizeof saved_rec);
    CHECK(counted_gone == 0, "held by the exec cut short, until");
    object(9);
    [exec_ref(9, 0, 401) finish:YES];
    settle();
    [task_objects(r, 1, YES) removeObjectForKey:@9];
    CHECK(counted_gone == 1, "the next exec let go of it (%u)", counted_gone);
}

static void test_function_record(void) {
    printf("a function record of another tag is refused before its library is read\n");
    uint8_t saved[sizeof surface_record];
    memcpy(saved, surface_record, sizeof saved);
    struct __attribute__((packed)) { uint64_t off; uint32_t len, tag, size; } h = {0x8000, 0, 0xe, 20};
    memcpy(surface_record, &h, sizeof h);
    CHECK(![r makeFunction:0x8000 task:1] && [r->complaints containsObject:@"function record of a tag other than 0xf refused"],
          "tag 0xe: refused");
    h.tag = HV_FUNCTION_RECORD_TAG;
    memcpy(surface_record, &h, sizeof h);
    CHECK(![r makeFunction:0x8000 task:1] && [r->complaints containsObject:@"library of 0 bytes refused"],
          "tag 0xf: taken, to the library's length (none here)");
    memcpy(surface_record, saved, sizeof saved);
}

/* gfx-fixes 8b: the colour probe sees a colour-matrix word change with the gamma table unchanged (Night Shift and
   Reduce White Point can work that way), and writes each different table the guest gives whole, once. */
static void test_colour_probe(void) {
    printf("the colour probe: the matrix looked at every swap, each table kept\n");
    char dir[] = "/tmp/henge-colourprobe-XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "a folder for the probe's files");
    uint8_t page[0x4000] = {0};
    test_display_page = page;
    for (int i = 0; i < 3 * 257; i++) gamma_table[i] = (uint32_t)(i % 257) * 16711935u; /* u32n, three identity curves */
    uint32_t sum = 0;
    for (int i = 0; i < 3 * 257; i++) sum += gamma_table[i];
    uint32_t a[10] = {0, 0, 0, 0, 0x7000, 0, sizeof gamma_table, 0, 257, sum};
    /* the probe's state, as at its start (cp holds an object, so it isn't cleared with memset) */
    cp.swaps = 0, cp.last_fp = cp.last_sum = 0, cp.last_len = 0, cp.searched = 0, cp.page_have = 0, cp.kept = 0;
    cp.on = 1, snprintf(cp.dir, sizeof cp.dir, "%s", dir);
    uint32_t one = 0x3f800000u, less = 0x3f400000u; /* the matrix's first word: 1.0, then 0.75 */
    memcpy(page + 0xa10, &one, 4);
    cp_note_swap(r, 5, a, 40);
    CHECK(cp.page_have && cp.page_prev[36] == one, "the first swap: the page read");
    char path[1200];
    snprintf(path, sizeof path, "%s/gamma-sum-%08x.bin", dir, sum);
    FILE *f = fopen(path, "rb");
    uint32_t back[3 * 257 + 1] = {0};
    size_t got = f ? fread(back, 1, sizeof back, f) : 0;
    if (f) fclose(f);
    CHECK(got == sizeof gamma_table && !memcmp(back, gamma_table, sizeof gamma_table), "the table written whole (%zu bytes)", got);
    for (int k = 0; k < 4; k++) cp_note_swap(r, 5, a, 40); /* nothing new: past the first three swaps' lines */
    memcpy(page + 0xa10, &less, 4);
    cp_note_swap(r, 5, a, 40); /* the matrix changed, the table not */
    CHECK(cp.page_prev[36] == less, "a matrix word changed with the table the same: seen");
    unlink(path);
    gamma_table[0] = 1, a[9] = sum + 1; /* a different table */
    cp_note_swap(r, 5, a, 40);
    snprintf(path, sizeof path, "%s/gamma-sum-%08x.bin", dir, sum + 1);
    CHECK(access(path, F_OK) == 0, "a different table: written too");
    unlink(path);
    a[9] = sum + 2, cp_note_swap(r, 5, a, 40), a[9] = sum + 1, cp_note_swap(r, 5, a, 40); /* back to it: written once only */
    CHECK(access(path, F_OK) != 0, "a table seen before: not written again");
    snprintf(path, sizeof path, "%s/gamma-sum-%08x.bin", dir, sum);
    CHECK(access(path, F_OK) != 0, "the first table: not written again");
    cp.on = 0, cp.swaps = 0, cp.page_have = 0, cp.kept = 0, cp.searched = 0;
    test_display_page = NULL;
    rmdir(dir);
}

@interface Tex12Device : NSObject
@property(nonatomic, strong) MTLTextureDescriptor *made;
@end
@implementation Tex12Device
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d { _made = [d copy]; return [FakeTexture new]; }
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)f { (void)f; return 64; }
@end

static void test_texture_type12(void) {
    printf("type 12 two-layout texture entries (v1, v2, and truncated)\n");
    id saved_dev = r->device;
    HvTexCaps saved_caps = r->caps;
    uint32_t saved_entry[3];
    memcpy(saved_entry, table[14], sizeof saved_entry);
    uint8_t saved_rec[sizeof tex12_record];
    memcpy(saved_rec, tex12_record, sizeof saved_rec);

    Tex12Device *dev = [Tex12Device new];
    r->device = (id)dev;
    r->caps.samples |= (1u << 1) | (1u << 4);

    /* Exact 176-byte type-12 layout seen from Chromium/ANGLE on macOS (2752x480 window -> 3072x512). */
    memset(tex12_record, 0, sizeof tex12_record);
    uint64_t length = 0x03000000ull, stride = 0x01800000ull, row = 0xc000ull;
    uint32_t vpage = 0x1cd9u, w = 3072, h = 512, d = 1;
    uint16_t mips = 1, slices = 1;
    memcpy(tex12_record + 0, &length, 8);
    memcpy(tex12_record + 8, &vpage, 4);
    uint8_t *l1 = tex12_record + 12;
    memcpy(l1 + 0, &mips, 2); memcpy(l1 + 2, &slices, 2);
    memcpy(l1 + 12, &stride, 8);
    l1[0x14] = 1, l1[0x15] = 1, l1[0x16] = 1, l1[0x17] = 4;
    uint8_t *m1 = l1 + 0x18;
    memcpy(m1 + 8, &stride, 8); memcpy(m1 + 0x10, &row, 8);
    memcpy(m1 + 0x18, &w, 4); memcpy(m1 + 0x1c, &h, 4); memcpy(m1 + 0x20, &d, 4);
    uint8_t *l2 = m1 + 36;
    memcpy(l2 + 0, &mips, 2); memcpy(l2 + 2, &slices, 2);
    memcpy(l2 + 4, &stride, 8); memcpy(l2 + 12, &stride, 8);
    l2[0x14] = 1, l2[0x15] = 1, l2[0x16] = 1, l2[0x17] = 1;
    uint8_t *rec = l2 + 0x18 + 36; /* offset 132 */
    uint32_t hdr[3] = {0x14, 44, 14};
    memcpy(rec, hdr, 12);
    uint8_t *desc = rec + 12;
    uint16_t format = 255 /* Depth24Unorm_Stencil8 -> Depth32Float_Stencil8 */, samples = 4, array = 1, options = 0x20;
    desc[0] = (uint8_t)MTLTextureType2DMultisample, desc[1] = MTLTextureUsageRenderTarget;
    memcpy(desc + 2, &format, 2);
    memcpy(desc + 4, &w, 4); memcpy(desc + 8, &h, 4); memcpy(desc + 12, &d, 4);
    memcpy(desc + 16, &mips, 2); memcpy(desc + 18, &samples, 2); memcpy(desc + 20, &array, 2); memcpy(desc + 22, &options, 2);

    table[14][0] = 12 | (176u << 8), table[14][1] = 0x9800, table[14][2] = 0;
    [r->tasks[1] removeObjectForKey:@14];
    HvObject *o = [r object:14 task:1];
    CHECK(o && o->object && o->kind == KIND_TEXTURE && dev.made != nil, "type 12 (176 bytes): built as KIND_TEXTURE");
    CHECK(dev.made.width == 3072 && dev.made.height == 512 && dev.made.sampleCount == 4 &&
          dev.made.pixelFormat == MTLPixelFormatDepth32Float_Stencil8,
          "type 12 descriptor decoded (%lux%lu samples %lu fmt %lu)",
          (unsigned long)dev.made.width, (unsigned long)dev.made.height,
          (unsigned long)dev.made.sampleCount, (unsigned long)dev.made.pixelFormat);

    /* Truncated type 12 (size 140 < 132 + 12 + 24): safely refused without overrun. */
    dev.made = nil;
    table[14][0] = 12 | (140u << 8), table[14][1] = 0x9800, table[14][2] = 0;
    [r->tasks[1] removeObjectForKey:@14];
    o = [r object:14 task:1];
    CHECK(o && !o->object && o->kind == KIND_NONE && dev.made == nil, "truncated type 12: safely refused");

    [r->tasks[1] removeObjectForKey:@14];
    r->device = saved_dev;
    r->caps = saved_caps;
    memcpy(table[14], saved_entry, sizeof saved_entry);
    memcpy(tex12_record, saved_rec, sizeof saved_rec);
}

static void test_dock_badge_metal_fixes(void) {
    CHECK(pixel_bytes((MTLPixelFormat)552) == 8 && pixel_bytes((MTLPixelFormat)553) == 8 &&
          pixel_bytes((MTLPixelFormat)554) == 4 && pixel_bytes((MTLPixelFormat)555) == 4,
          "pixel_bytes supports XR10 formats 552..555");
    CHECK(is_srgb_format(71) && is_srgb_format(81) && is_srgb_format(553) && is_srgb_format(555) && !is_srgb_format(70),
          "is_srgb_format matches sRGB formats");

    uint8_t v2[0x27] = {0};
    uint16_t fmt = 70, mips = 1, samples = 1, array = 1;
    uint32_t usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, w = 16, h = 16, depth = 1;
    v2[0] = (uint8_t)MTLTextureType2D;
    memcpy(v2 + 1, &fmt, 2);
    memcpy(v2 + 3, &usage, 4);
    memcpy(v2 + 7, &w, 4);
    memcpy(v2 + 11, &h, 4);
    memcpy(v2 + 15, &depth, 4);
    memcpy(v2 + 19, &mips, 2);
    memcpy(v2 + 21, &samples, 2);
    memcpy(v2 + 23, &array, 2);
    v2[0x23] = (uint8_t)MTLTextureSwizzleBlue;
    v2[0x24] = (uint8_t)MTLTextureSwizzleGreen;
    v2[0x25] = (uint8_t)MTLTextureSwizzleRed;
    v2[0x26] = (uint8_t)MTLTextureSwizzleAlpha;
    MTLTextureDescriptor *sw_t = descriptor_v2(v2);
    CHECK(sw_t.swizzle.red == MTLTextureSwizzleRed && sw_t.swizzle.blue == MTLTextureSwizzleBlue,
          "descriptor_v2 clears non-identity swizzle for ShaderWrite");

    MTLTextureDescriptor *srgb_t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                                                                      width:16
                                                                                     height:16
                                                                                  mipmapped:NO];
    srgb_t.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    srgb_t.allowGPUOptimizedContents = YES;
    id<MTLDevice> saved_dev = r->device;
    Tex12Device *dev = [Tex12Device new];
    r->device = (id<MTLDevice>)dev;
    [r refuse:srgb_t what:@"srgb linear test" buffer:4096 offset:0 row:256];
    CHECK(!srgb_t.allowGPUOptimizedContents &&
          (srgb_t.usage & MTLTextureUsageShaderWrite) == 0 &&
          (srgb_t.usage & MTLTextureUsageShaderRead) != 0,
          "refuse: clears allowGPUOptimizedContents for linear buffers and strips ShaderWrite on sRGB");
    r->device = saved_dev;
}

@interface StaleTex : NSObject
@property(nonatomic) NSUInteger textureType, pixelFormat, width, height, depth, mipmapLevelCount, sampleCount, arrayLength, usage;
@property(nonatomic) NSUInteger cpuCacheMode, storageMode, hazardTrackingMode, parentRelativeLevel, parentRelativeSlice;
@property(nonatomic) BOOL isFramebufferOnly;
@property(nonatomic, strong) id parentTexture;
@end
@implementation StaleTex
- (instancetype)init {
    if ((self = [super init])) {
        _textureType = MTLTextureType2D, _pixelFormat = MTLPixelFormatBGRA8Unorm;
        _width = 64, _height = 64, _depth = 1, _mipmapLevelCount = 4, _sampleCount = 1, _arrayLength = 1;
        _usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        _storageMode = MTLStorageModePrivate;
    }
    return self;
}
@end
@interface StaleBlit : NSObject
@end
@implementation StaleBlit
- (void)copyFromBuffer:(id)b sourceOffset:(NSUInteger)o sourceBytesPerRow:(NSUInteger)r sourceBytesPerImage:(NSUInteger)i
            sourceSize:(MTLSize)s toTexture:(id)t destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl
     destinationOrigin:(MTLOrigin)orig options:(MTLBlitOption)opt {}
- (void)copyFromTexture:(id)a sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
              toTexture:(id)b destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)orig {}
- (void)generateMipmapsForTexture:(id)t {}
@end
@interface StaleCompute : NSObject
@end
@implementation StaleCompute
- (void)setTextures:(const id __unsafe_unretained *)textures withRange:(NSRange)range {}
@end
@interface StaleRender : NSObject
@end
@implementation StaleRender
- (void)useResources:(const id __unsafe_unretained *)resources count:(NSUInteger)count usage:(MTLResourceUsage)usage {}
- (void)useResources:(const id __unsafe_unretained *)resources count:(NSUInteger)count usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages {}
@end

static void test_guest_stale_writes(void) {
    printf("GPU writes via blit/compute/useResources mark targets and parents guest_stale\n");
    HvObject *heap = object(10), *tex = object(11), *view = object(12), *buf = object(13), *ro = object(14);
    heap->kind = KIND_HEAP, heap->object = [NSObject new];
    tex->kind = KIND_TEXTURE, tex->object = [StaleTex new], tex->parent = heap;
    view->kind = KIND_TEXTURE, view->object = [StaleTex new], view->parent = tex;
    buf->kind = KIND_BUFFER, buf->object = [FakeBuffer new];
    ro->kind = KIND_TEXTURE, ro->object = [StaleTex new];

    /* blit 300: buffer to view marks view, parent texture, and heap guest_stale */
    uint8_t b300[0x58] = {0};
    uint32_t bref = 13, tref = 12;
    uint64_t row = 64, img = 16 * 64;
    MTLSize sz = {16, 16, 1};
    MTLOrigin orig = {0, 0, 0};
    memcpy(b300 + 0, &bref, 4); memcpy(b300 + 4, &tref, 4);
    memcpy(b300 + 0x10, &row, 8); memcpy(b300 + 0x18, &img, 8);
    memcpy(b300 + 0x20, &sz, sizeof sz); memcpy(b300 + 0x38, &orig, sizeof orig);
    StaleBlit *be = [StaleBlit new];
    dispatch_sync(r->worker, ^{ [r blit:(id<MTLBlitCommandEncoder>)be tag:300 rec:(Rec){b300, sizeof b300} task:1]; });
    CHECK(view->guest_stale && tex->guest_stale && heap->guest_stale, "blit 300: view, base texture, and heap marked guest_stale");

    /* blit 303: texture to texture marks destination guest_stale (and not source) */
    heap->guest_stale = NO, tex->guest_stale = NO, view->guest_stale = NO, ro->guest_stale = NO;
    uint8_t b303[0x58] = {0};
    uint32_t sref = 14, dref = 12;
    memcpy(b303 + 0, &sref, 4); memcpy(b303 + 4, &dref, 4);
    memcpy(b303 + 0x20, &sz, sizeof sz);
    dispatch_sync(r->worker, ^{ [r blit:(id<MTLBlitCommandEncoder>)be tag:303 rec:(Rec){b303, sizeof b303} task:1]; });
    CHECK(view->guest_stale && tex->guest_stale && heap->guest_stale && !ro->guest_stale,
          "blit 303: destination chain marked guest_stale, source unchanged");

    /* blit 307: generateMipmaps marks texture and parent guest_stale */
    heap->guest_stale = NO, tex->guest_stale = NO, view->guest_stale = NO;
    uint32_t b307[2] = {12, 0};
    dispatch_sync(r->worker, ^{ [r blit:(id<MTLBlitCommandEncoder>)be tag:307 rec:(Rec){(const uint8_t *)b307, sizeof b307} task:1]; });
    CHECK(view->guest_stale && tex->guest_stale && heap->guest_stale, "blit 307: mipmap target and parents marked guest_stale");

    /* compute 206: ShaderWrite marks guest_stale; ShaderRead-only does not */
    heap->guest_stale = NO, tex->guest_stale = NO, view->guest_stale = NO, ro->guest_stale = NO;
    ((StaleTex *)view->object).usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    ((StaleTex *)ro->object).usage = MTLTextureUsageShaderRead;
    uint32_t c206[4] = {0, 2, 12, 14};
    StaleCompute *ce = [StaleCompute new];
    dispatch_sync(r->worker, ^{ [r compute:(id<MTLComputeCommandEncoder>)ce tag:206 rec:(Rec){(const uint8_t *)c206, sizeof c206} task:1]; });
    CHECK(view->guest_stale && tex->guest_stale && heap->guest_stale && !ro->guest_stale,
          "compute 206: ShaderWrite texture marked guest_stale, read-only not");

    /* render 135: useResources with write marks guest_stale; read-only does not */
    heap->guest_stale = NO, tex->guest_stale = NO, view->guest_stale = NO, ro->guest_stale = NO;
    StaleRender *re = [StaleRender new];
    uint32_t r135_w[3] = {1, MTLResourceUsageWrite, 12};
    uint32_t r135_r[3] = {1, MTLResourceUsageRead, 14};
    dispatch_sync(r->worker, ^{
        [r render:(id<MTLRenderCommandEncoder>)re tag:135 rec:(Rec){(const uint8_t *)r135_w, sizeof r135_w} task:1];
        [r render:(id<MTLRenderCommandEncoder>)re tag:135 rec:(Rec){(const uint8_t *)r135_r, sizeof r135_r} task:1];
    });
    CHECK(view->guest_stale && tex->guest_stale && heap->guest_stale && !ro->guest_stale,
          "render 135: write resource marked guest_stale, read-only not");

    /* HV_PVG_SURFACE_LOG gates per-surface logging in env */
    unsetenv("HV_PVG_SURFACE_LOG");
    read_env(r);
    CHECK(!r->env.surface_log, "surface_log defaults NO when HV_PVG_SURFACE_LOG is unset");
    setenv("HV_PVG_SURFACE_LOG", "1", 1);
    read_env(r);
    CHECK(r->env.surface_log, "surface_log enabled when HV_PVG_SURFACE_LOG is set");
    unsetenv("HV_PVG_SURFACE_LOG");
    read_env(r);

    [r->tasks[1] removeObjectForKey:@10];
    [r->tasks[1] removeObjectForKey:@11];
    [r->tasks[1] removeObjectForKey:@12];
    [r->tasks[1] removeObjectForKey:@13];
    [r->tasks[1] removeObjectForKey:@14];
}

#ifndef HV_PVG_TEST_NO_METAL
@interface StaleCB : NSObject
@end
@implementation StaleCB
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d { (void)d; return [StaleRender new]; }
@end

static void test_guest_stale_render_pass(void) {
    printf("render pass attachments and resolve views mark targets and parents guest_stale\n");
    HvTexCaps saved_caps = r->caps;
    r->caps.samples |= (1u << 1) | (1u << 4);
    r->caps.color_bytes = 128, r->caps.implicit_bytes = 256 << 10;

    HvObject *heap = object(10), *tex = object(11), *view = object(12), *res = object(13), *dc = object(14);
    heap->kind = KIND_HEAP, heap->object = [NSObject new];
    tex->kind = KIND_TEXTURE, tex->object = [StaleTex new], tex->parent = heap;
    view->kind = KIND_TEXTURE, view->object = [StaleTex new], view->parent = tex;
    ((StaleTex *)view->object).sampleCount = 4;
    ((StaleTex *)view->object).textureType = MTLTextureType2DMultisample;
    ((StaleTex *)view->object).mipmapLevelCount = 1;
    res->kind = KIND_TEXTURE, res->object = [StaleTex new];
    ((StaleTex *)res->object).mipmapLevelCount = 1;
    dc->kind = KIND_TEXTURE, dc->object = [StaleTex new];
    ((StaleTex *)dc->object).mipmapLevelCount = 1;

    uint8_t pass[0x248] = {0};
    uint32_t c0_tex = 12, c0_res = 13, c0_dc = 14;
    uint16_t load_clear = MTLLoadActionClear, store_resolve = MTLStoreActionMultisampleResolve;
    uint16_t load_load = MTLLoadActionLoad, store_dc = MTLStoreActionDontCare;
    memcpy(pass + 0x4c + 0, &c0_tex, 4);
    memcpy(pass + 0x4c + 4, &c0_res, 4);
    memcpy(pass + 0x4c + 20, &load_clear, 2);
    memcpy(pass + 0x4c + 22, &store_resolve, 2);
    uint64_t layers = 1, pw = 64, ph = 64;
    memcpy(pass + 0x230, &layers, 8); memcpy(pass + 0x238, &pw, 8); memcpy(pass + 0x240, &ph, 8);

    StaleCB *cb = [StaleCB new];
    __block id<MTLRenderCommandEncoder> enc = nil;
    dispatch_sync(r->worker, ^{ enc = [r beginPass:(Rec){pass, sizeof pass} task:1 commandBuffer:(id<MTLCommandBuffer>)cb]; });
    CHECK(enc != nil, "MSAA render pass begun");
    CHECK(view->guest_stale && tex->guest_stale && heap->guest_stale && res->guest_stale,
          "beginPass: stored/resolved attachments and parents marked guest_stale");

    uint8_t pass_dc[0x248] = {0};
    memcpy(pass_dc + 0x4c + 0, &c0_dc, 4);
    memcpy(pass_dc + 0x4c + 20, &load_load, 2);
    memcpy(pass_dc + 0x4c + 22, &store_dc, 2);
    memcpy(pass_dc + 0x230, &layers, 8); memcpy(pass_dc + 0x238, &pw, 8); memcpy(pass_dc + 0x240, &ph, 8);
    __block id<MTLRenderCommandEncoder> enc_dc = nil;
    dispatch_sync(r->worker, ^{ enc_dc = [r beginPass:(Rec){pass_dc, sizeof pass_dc} task:1 commandBuffer:(id<MTLCommandBuffer>)cb]; });
    CHECK(enc_dc != nil && !dc->guest_stale, "beginPass: load+DontCare pass leaves attachment guest_stale NO");

    r->caps = saved_caps;
    [r->tasks[1] removeObjectForKey:@10];
    [r->tasks[1] removeObjectForKey:@11];
    [r->tasks[1] removeObjectForKey:@12];
    [r->tasks[1] removeObjectForKey:@13];
    [r->tasks[1] removeObjectForKey:@14];
}
#endif

#ifdef HV_PVG_TEST_NO_METAL /* test_pvg_present_nometal: these check pvg_metal.m against Metal's own descriptor objects,
                               which that build hasn't got (tests/pvg_nometal_stubs.m); test_pvg_present_nogpu runs them */
#define METAL_OBJECTS(test) printf("%s: skipped, it needs Metal's own descriptors (run in test_pvg_present_nogpu)\n", #test)
#else
#define METAL_OBJECTS(test) test()
#endif
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    @autoreleasepool {
        unsetenv("HV_PVG_SYNC");
        r = [TestReplay new];
        q = [TestQueue new];
        r->queue = (id<MTLCommandQueue>)q;
        r->worker = dispatch_queue_create("test.worker", DISPATCH_QUEUE_SERIAL);
        r->presenter = dispatch_queue_create("test.present", DISPATCH_QUEUE_SERIAL);
        r->screen_lock = OS_UNFAIR_LOCK_INIT;
        made_lock = &r->screen_lock;
        r->layer = (CAMetalLayer *)[NSObject new]; /* the presenter is the test's: it never draws */
        r->tasks[1] = [HvTaskObjects new];
        r->complaints = [NSMutableSet new];
        r->mapper_q = dispatch_queue_create("test.mapper", DISPATCH_QUEUE_SERIAL);
        r->complete_q = test_complete_q = dispatch_queue_create("test.complete", DISPATCH_QUEUE_SERIAL);
        r->writeback = [NSMutableArray new];
#ifndef HV_PVG_TEST_HOOKS
        (void)argc, (void)argv;
        setenv("HV_PVG_ENCODE_ONLY", "1", 1);
        test_refused();
#else
        cb_wait_test = fake_cb_wait;
        if (argc > 1 && !strcmp(argv[1], "hold")) { /* the hold's test alone: nothing of it reaches a GPU */
            test_hold();
            printf("%d checks, %d failures\n", checks, failures);
            return failures != 0;
        }
        test_redraw();
        test_fence();
        test_swap_waits_for_presenter();
        test_swap_routed();
        test_map_runs();
        test_cursor_scaled();
        test_tile_size_pages();
        test_texture_descriptor();
        METAL_OBJECTS(test_sampler_made);
        test_command_checks();
        test_video();
        test_video_unsaid();
        test_video_reader_not_behind_compare();
        test_video_gpu_write();
        test_video_back();
        test_video_shared();
        test_surface_misaligned();
        test_map_pin_first();
        test_video_kept();
        test_exec_no_memory();
        test_video_key();
        test_breaker_classes();
        test_inflight_cap();
        test_drawable_wait();
        test_breaker();
        test_save_waits();
        test_hold();
        test_gpu_never_done();
        test_try_again();
        test_clamp_fences();
        test_clamp_exec_cap();
        METAL_OBJECTS(test_stage_kinds);
        test_forget_deleted();
        test_forget_logged();
        test_task_commands();
        test_unmap_lets_go();
    test_patch_bounds();
        test_function_record();
        test_colour_probe();
        test_env_once();
        test_lookup_once();
        METAL_OBJECTS(test_pipeline_timed);
        test_heap_buffer_refused();
        test_info_null();
    test_unmap_on_screen();
    test_presenter_commits();
    test_stale_map();
    test_exec_throw();
        test_view_chains();
        test_texture_type12();
        test_dock_badge_metal_fixes();
        test_guest_stale_writes();
        METAL_OBJECTS(test_guest_stale_render_pass);
        METAL_OBJECTS(test_pipeline_checks);
        METAL_OBJECTS(test_building_note);
#endif
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
