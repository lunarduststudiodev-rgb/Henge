#ifndef HENGEVM_PVG_LITE_H
#define HENGEVM_PVG_LITE_H

#include <stdint.h>

#include "gicv3.h"

/* HengeVM's own graphics device, for hosts without
   ParavirtualizedGraphics.framework (iOS). It speaks the protocol of Apple's
   guest driver (AppleParavirtGPU) so the driver starts and its commands
   complete; pvg_metal.m runs what they draw. */
typedef struct HvPvgLite HvPvgLite;

HvPvgLite *hv_pvg_lite_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa, int gfx_irq, int iosfc_irq);
/* Completes doorbells rung since the last call. Device lock held. */
void hv_pvg_lite_poll(HvPvgLite *p);
/* The newest frame the CPU path drew, BGRA, width * 4 bytes per row: copied into to when cap holds it (0), else
   only its size given (1); -1 when there is none. Any thread: the copy is taken under the frame's lock. */
int hv_pvg_lite_frame(HvPvgLite *p, uint8_t *to, size_t cap, uint32_t *width, uint32_t *height);
/* How long each graphics stage has been held up, ms (0: not now), for the freeze recorder; any thread:
   QUEUED: the oldest command handed to the Metal worker and not begun; WORKER: the one it runs; GPU: GPU work in
   flight with none finished since; BARRIER: the longest a channel (or the root ring) has waited for another's work;
   DRAWABLE: the presenter wanting a drawable from the screen's layer and not having one (a frame is counted only
   once it is presented). */
enum { HV_PVG_STAGE_QUEUED, HV_PVG_STAGE_WORKER, HV_PVG_STAGE_GPU, HV_PVG_STAGE_BARRIER, HV_PVG_STAGE_DRAWABLE, HV_PVG_STAGES };
void hv_pvg_lite_stages(HvPvgLite *p, uint32_t ms[HV_PVG_STAGES]);
/* The GPU trace is on: HV_PVG_TRACE_FILE names a file that exists (looked for at most once a second). Any thread. */
int hv_pvg_tracing(void);

/* Between the device and its Metal side (pvg_metal.m). */
typedef struct HvPvgMetal HvPvgMetal;
HvPvgMetal *hv_pvg_metal_create(HvPvgLite *lite);
/* hv_pvg_lite_stages's all but BARRIER, from the worker, the GPU and the presenter. Any thread. */
void hv_pvg_metal_stages(HvPvgMetal *m, uint32_t ms[HV_PVG_STAGES]);
void hv_pvg_metal_submit(HvPvgMetal *m, HvPvgLite *lite, uint32_t channel, uint32_t *command, uint32_t length);
/* A delete (0x20, 0x25 or 0x28) that came on the root ring: forgotten on the worker, in order with the channel work
   handed over before. Takes the malloc'd command. */
void hv_pvg_metal_forget(HvPvgMetal *m, uint32_t *command, uint32_t length);
/* For a fatal signal's handler: the pipeline this thread was building, if it was (async-signal-safe; to stderr). */
void hv_pvg_metal_crash_note(void);
int hv_pvg_metal_snapshot(HvPvgMetal *m, uint8_t *bgra, uint32_t width, uint32_t height);
int hv_pvg_lite_snapshot(HvPvgLite *p, uint8_t *bgra, uint32_t width, uint32_t height);
int hv_pvg_metal_frame(HvPvgMetal *m, uint8_t *to, size_t cap, uint32_t *width, uint32_t *height);
/* Suspend lab (Phase 0): logs what graphics data lives only on the GPU, and with readback, what reading it all
   back costs. Waits up to 8 s for the drawing queue. Measurement only. */
void hv_pvg_metal_lab(HvPvgMetal *m, int readback, const char *when);
void hv_pvg_lite_lab(HvPvgLite *p, int readback, const char *when);
int hv_pvg_lite_task_read(HvPvgLite *p, uint32_t task, uint64_t offset, void *to, uint32_t length);
uint32_t hv_pvg_lite_task_page(HvPvgLite *p, uint32_t task, uint64_t offset);
uint8_t *hv_pvg_lite_page(HvPvgLite *p, uint32_t guest_page);
void *hv_pvg_lite_phys(HvPvgLite *p, uint64_t pa, uint64_t length);
uint64_t hv_pvg_lite_task_objects(HvPvgLite *p, uint32_t task);
/* The Metal side's worker reached a command (op, its arguments after the barriers, their bytes): a define (0x38) or
   object list (0x44) is what the task reads above use from now on. Only the worker calls it, in the guest's order. */
void hv_pvg_lite_task_command(HvPvgLite *p, uint32_t op, const uint32_t *args, uint32_t n);
/* A surface's descriptor (1 KB), or NULL if the mapper has none for it. The id as the guest gave it, all 64 bits: an
   id past 32 bits is none of the mapper's, not its low half. */
const uint8_t *hv_pvg_lite_surface(HvPvgLite *p, uint64_t mapping);
void hv_pvg_metal_unmap(HvPvgMetal *m, uint32_t mapping);
/* The mapper mapped this id (again): a surface may be made from its descriptor, and one still there from before (its
   unmap passed over) is dropped. One unmapped and not mapped again may name pages the guest has reused, so none is
   (pvg_metal.m's unmapped). Reset: the mapper's walk passed over entries (a ring restart, an index that isn't a ring
   position, a ring not in guest memory), so they may have mapped any of them: all may be made again. */
void hv_pvg_metal_map(HvPvgMetal *m, uint32_t mapping);
void hv_pvg_metal_mapper_reset(HvPvgMetal *m);
/* Mapping ids are 1 to HV_PVG_MAPPING_IDS - 1 (pvg_lite.c's mapping_id). The ids unmapped and not mapped again, a bit
   each (bit id % 8 of byte id / 8), as the mapper knows them: its whole state, which the Metal side takes as its own
   copy when it is made (and so on a restore, before the saved surfaces are made again), ahead of any map or unmap
   after. An id in neither state (its bit clear) may be mapped or not: one is made from its descriptor as ever. */
#define HV_PVG_MAPPING_IDS 0x2000u
void hv_pvg_metal_set_unmapped(HvPvgMetal *m, const uint8_t unmapped[HV_PVG_MAPPING_IDS / 8]);
/* The mapper's completed index may move to index once the GPU work handed over so far is done: the Metal side
   then calls hv_pvg_lite_mapper_done(lite, index, gen) from a thread no device lock holder waits for. */
void hv_pvg_metal_mapper_fence(HvPvgMetal *m, uint64_t index, uint64_t gen);
void hv_pvg_lite_mapper_done(HvPvgLite *p, uint64_t index, uint64_t gen);
void hv_pvg_metal_cursor_moved(HvPvgMetal *m);
void hv_pvg_metal_hide_pointer(HvPvgMetal *m, int hide);
/* Frames go to this CAMetalLayer from now on, drawn by the GPU. Any thread. */
void hv_pvg_metal_set_layer(HvPvgMetal *m, void *layer);
uint64_t hv_pvg_metal_gpu_frames(HvPvgMetal *m);
/* 1 once the GPU failed the guest's work (a hang its watchdog stopped, a fault, or failures again and again): the
   guest's graphics are not run again in this run, so it can stall the host's GPU once at most. For the app to tell
   the user. Any thread. */
int hv_pvg_metal_gpu_faulted(HvPvgMetal *m);
/* The guest's graphics run again after that, with no limit (the tests; the app uses hv_pvg_metal_gpu_try_again). */
void hv_pvg_metal_gpu_rearm(HvPvgMetal *m);
/* Where the guest's graphics stand, for the app to tell the person (any thread):
   RUNS; STOPPED (the GPU failed its work: it can be tried again); STOPPED_SETTLING (the same, but some of its work is
   still on the GPU, a hang its watchdog has yet to end: try again in a moment); STOPPED_FOR_GOOD (tried again as many
   times as a run allows, or this build runs none of it: only restarting the Mac brings them back). */
enum { HV_PVG_GPU_RUNS = 0, HV_PVG_GPU_STOPPED = 1, HV_PVG_GPU_STOPPED_SETTLING = 2, HV_PVG_GPU_STOPPED_FOR_GOOD = 3 };
int hv_pvg_metal_gpu_state(HvPvgMetal *m);
/* The person's "Try Again" (only when STOPPED): the guest's graphics run again, a limited number of times a run, for
   the same work may hang the GPU again. 0 they run again; -1 not (not STOPPED). Any thread. */
int hv_pvg_metal_gpu_try_again(HvPvgMetal *m);
/* 1 once this OS's Metal has refused GPU code macOS sent, even relabelled for it (macOS 15 on iPadOS 17): parts or all
   of the Mac's screen cannot be drawn here. */
int hv_pvg_metal_gpu_code_refused(HvPvgMetal *m);
/* An auto-save's hold (core/ramfile.h: hv_ramfile_claim_gpu, before the GPU runs again). Called with the CPUs paused,
   once the GPU's work is done (no command buffer in flight). Parks the replay's worker, which is the only thing that
   then writes the guest's pages through a GPU mapping (exec copy-backs, video copy-backs, new GPU
   work) or maps more of them, until hv_pvg_metal_release; and waits for what the completions handed their own queue
   (stamps and video copy-backs, which write the guest's pages after the GPU is done). Returns once both are so: at
   once when nothing is queued ahead, else within wait_ns.
   No vCPU waits for the worker (they only queue work for it), so the hold stops the guest's graphics only, at most
   HV_PVG_HOLD_MAX (2 s): then they go on, and the release says so. 0 held; -1 not held (one is already, or the worker
   was not parked in time), and then the save can't count on its claim of the GPU's pages.
   Not used yet: nothing calls this, hv_pvg_lite_hold or hv_ramfile_claim_gpu so far, so today's auto-save neither
   parks the worker nor waits for the completions' queue. It is ready for the auto-save that claims the GPU's pages. */
int hv_pvg_metal_hold(HvPvgMetal *m, uint64_t wait_ns);
/* Ends the hold (any thread; after hv_ramfile_claim_gpu): 0 if the worker stayed parked all along, -1 if it was let go
   before (HV_PVG_HOLD_MAX), or there was no hold. Safe whatever the GPU breaker's state. */
int hv_pvg_metal_release(HvPvgMetal *m);
int hv_pvg_lite_hold(HvPvgLite *p, uint64_t wait_ns);
int hv_pvg_lite_release(HvPvgLite *p);
int hv_pvg_lite_gpu_code_refused(HvPvgLite *p);
/* 1 once macOS has shown its pointer: the login window or the desktop is up (the boot screens have none). */
int hv_pvg_metal_pointer_shown(HvPvgMetal *m);
/* The size of what macOS is showing now, in pixels: it changes when the mode does. 0, or -1 before anything. */
int hv_pvg_metal_screen_size(HvPvgMetal *m, uint32_t *w, uint32_t *h);
/* The Mac's screen in the pixels its pointer positions are in (the display's mode), and in its points. */
int hv_pvg_lite_screen_size(HvPvgLite *p, uint32_t *w, uint32_t *h);
int hv_pvg_lite_screen_points(HvPvgLite *p, uint32_t *w, uint32_t *h);
/* The size of the picture macOS shows now (fitted to the screen as it is), in its own pixels. */
int hv_pvg_lite_picture_size(HvPvgLite *p, uint32_t *w, uint32_t *h);
/* The panel is Retina-dense, so macOS offers it larger and smaller looks: sizes other than the panel's own. */
int hv_pvg_lite_retina(void);
/* The display mode a picture of this size is shown on: the pointer's and the cursor's pixels. */
void hv_pvg_lite_pointer_space(uint32_t surface_w, uint32_t surface_h, uint32_t *w, uint32_t *h);
void hv_pvg_lite_task_pages(HvPvgLite *p, uint32_t task, uint64_t first, uint64_t n, uint32_t *out);
void hv_pvg_lite_set_layer(HvPvgLite *p, void *layer);
/* Hides the Mac's pointer from the screen (while a finger is the pointer), or shows it again. Any thread. */
void hv_pvg_lite_hide_pointer(HvPvgLite *p, int hide);
uint64_t hv_pvg_lite_gpu_frames(HvPvgLite *p);
int hv_pvg_lite_gpu_faulted(HvPvgLite *p);
void hv_pvg_lite_gpu_rearm(HvPvgLite *p);
int hv_pvg_lite_gpu_state(HvPvgLite *p);
int hv_pvg_lite_gpu_try_again(HvPvgLite *p);
int hv_pvg_lite_pointer_shown(HvPvgLite *p);
/* 1 once the guest has turned the display or its surface mapper on. */
int hv_pvg_lite_in_use(HvPvgLite *p);

/* ---- Suspend ----
   The device's state as bytes in its own format, which the board puts in the snapshot's "PVG " section
   (kept apart from core/snapshot.c, so this device needs nothing more than it did to link).
   What the GPU alone holds is read back: textures whose GPU copy is newer go back to their own guest
   pages (as the guest's own "synchronize" does), and render targets with no guest memory, and heaps with
   none, go into the bytes. On restore the Metal side starts empty, makes every texture, heap and surface
   again from the guest's descriptions (in the restored RAM), and fills them; shaders and pipelines it
   makes again when they are next used, as it always does. */
typedef struct HvPvgBlob {
    uint8_t *p;
    size_t len, cap;
    int bad; /* out of memory */
} HvPvgBlob;
void hv_pvg_blob_put(HvPvgBlob *b, const void *v, size_t n);
typedef struct HvPvgReader {
    const uint8_t *p;
    size_t len, at;
    int bad; /* read past the end */
} HvPvgReader;
void hv_pvg_blob_get(HvPvgReader *r, void *v, size_t n);
/* Where the GPU's part of a save goes as it is made: as much as the GPU holds (gigabytes on a big screen with
   many windows), so never built in memory, but written to the snapshot file in pieces. write: 0, or -1.
   room (may be NULL): asked once, before any of it is written, whether bytes more will fit; 0 if so, else -1
   with why said for the person. */
typedef struct HvPvgSink {
    int (*write)(void *ctx, const void *p, size_t n);
    void *ctx;
    int (*room)(void *ctx, uint64_t bytes, char *why, size_t why_len);
} HvPvgSink;
/* The GPU's part of a snapshot, kept from the load to the restore; release(ctx) lets go of it. */
typedef struct HvPvgBytes {
    const uint8_t *p;
    size_t len;
    void (*release)(void *ctx);
    void *ctx;
} HvPvgBytes;
/* The guest stopped and the device lock held. Waits for the GPU's work (to deadline_ns, CLOCK_MONOTONIC),
   then reads back what only the GPU holds into metal, and the device's own state into out. *metal_len is how
   much went into metal (UINT64_MAX: no GPU side yet, nothing written). Guest RAM changes (pages and finished
   stamps), so the board saves RAM after this. 0, or -1 with why. */
/* mapper (NULL: not wanted): the mapper's state (the ids unmapped), filled once the walk it had left is done, so it
   agrees with what metal holds. The snapshot's PVGS section, version HV_PVG_MAPPER_VERSION. */
int hv_pvg_lite_save(HvPvgLite *p, HvPvgBlob *out, const HvPvgSink *metal, uint64_t *metal_len, HvPvgBlob *mapper,
                     uint64_t deadline_ns, char *why, size_t why_len);
#define HV_PVG_MAPPER_VERSION 1
/* Into a device the guest has not used, with the guest's RAM already loaded. Checks and takes the device's
   registers; the Metal part waits for hv_pvg_lite_restored. metal is the GPU's part saved beside it (NULL for
   a snapshot from before it was kept apart, which has it inside data); it is taken either way. 0, or -1 (and
   the device is as it was). */
int hv_pvg_lite_load(HvPvgLite *p, const uint8_t *data, size_t len, HvPvgBytes *metal);
/* After hv_pvg_lite_load took the rest: the mapper's saved state, its section's version (-1: the snapshot has none,
   an older Henge's) and bytes (NULL: not read). One that is absent, of another version or damaged leaves every id unknown, as a snapshot
   from before it was kept: never a reason to refuse the resume. Says which, in one line. */
void hv_pvg_lite_load_mapper(HvPvgLite *p, const uint8_t *data, size_t len, int version);
/* Before anything is loaded: 0 if hv_pvg_lite_load would take this (it is whole, of this version, the GPU's
   part too: metal, or NULL for an older one with it inside), -1 if it would surely refuse it. Changes nothing. */
int hv_pvg_lite_check(const uint8_t *data, size_t len, const uint8_t *metal, size_t metal_len);
/* Once the whole snapshot has loaded: Metal makes the saved objects again and shows the saved frame. */
void hv_pvg_lite_restored(HvPvgLite *p);
/* The Metal side of the above (pvg_metal.m). Its part of the bytes is its own. */
int hv_pvg_metal_save(HvPvgMetal *m, const HvPvgSink *out, uint64_t deadline_ns, char *why, size_t why_len);
int hv_pvg_metal_check(const uint8_t *data, size_t len); /* 0 if the bytes are whole and make sense */
/* Before the CPUs run, not on the worker: done when it returns. Takes the bytes and lets go of them. */
void hv_pvg_metal_restore(HvPvgMetal *m, HvPvgBytes bytes);
void hv_pvg_bytes_release(HvPvgBytes *b); /* nothing if there are none */
/* Tests: a texture's first slice and level, read (write 0) or written (write 1). 0, or -1. Waits for the GPU. */
int hv_pvg_metal_test_texture(HvPvgMetal *m, uint32_t task, uint32_t ref, void *bytes, uint32_t row, int write);
/* Tests: a buffer's or heap's first len bytes on the GPU, read or written (0, or -1); an info question (tags 450...) as an exec asks it. */
int hv_pvg_metal_test_bytes(HvPvgMetal *m, uint32_t task, uint32_t ref, void *bytes, uint32_t len, int write);
void hv_pvg_metal_test_info(HvPvgMetal *m, uint32_t task, uint32_t tag, const void *rec, uint32_t size);
/* The tests' build of pvg_metal.m only (HV_PVG_TEST_HOOKS), from here to hv_pvg_metal_test_indirect.
   Tests: one object of each kind (buffer, texture, sampler, pipelines...) at refs first.. of a task's
   table, for fuzzing records that name objects. How many, or -1. */
int hv_pvg_metal_test_objects(HvPvgMetal *m, uint32_t task, uint32_t first);
/* For tests: the guest's work as if a command buffer of it came back failed with this MTLCommandBufferError code,
   without the GPU doing anything. */
void hv_pvg_metal_test_gpu_failed(HvPvgMetal *m, long code);
/* Tests: indirect draws drawn from clamped arguments so far, and the last clamped pass's jobs (HvClampJob) */
uint64_t hv_pvg_metal_test_indirect(HvPvgMetal *m, void *jobs, uint32_t max, uint32_t *n);
HvPvgMetal *hv_pvg_lite_metal(HvPvgLite *p);
void hv_pvg_lite_barrier_stats(HvPvgLite *p, uint64_t out[4]);
void hv_pvg_lite_set_vblank(HvPvgLite *p, int on);
#include <stdio.h>
void hv_pvg_lite_debug(HvPvgLite *p, FILE *f);
/* The display's shared state page, or NULL before the guest named it. */
const uint8_t *hv_pvg_lite_display_page(HvPvgLite *p);
/* A command the Metal side hands back in order (display ones): 0 for a display command not taken (shorter than
   Apple's host takes, or a port past the one display, so nothing is presented or drawn for it), else 1. */
int hv_pvg_lite_command(HvPvgLite *p, const uint32_t *command, uint32_t length);
void hv_pvg_lite_complete(HvPvgLite *p, uint32_t channel, uint32_t stamp);

/* The size macOS is told its screen is. Set before the machine starts; default 1920 x 1080. */
void hv_pvg_lite_set_mode(uint32_t width, uint32_t height);
void hv_pvg_lite_set_size_mm(uint32_t width_mm, uint32_t height_mm);
/* The mode macOS will start in, from the two above, and its pixels per point (1 or 2). -1 before set_mode. */
int hv_pvg_lite_start_mode(uint32_t *width, uint32_t *height, uint32_t *scale);

#endif
