/* The Metal side (devices/pvg_metal.m) stood in for, for the tests that build the display device itself: they include
   devices/pvg_lite.c, then this. No GPU, nor Metal linked. Every hook pvg_lite.c calls is here, once, so a new hook
   gets its stand-in in one place and every such test has it. tools/check_pvg_standins.sh (make test) fails when
   pvg_lite.o calls a hook this file lacks, or when a test that includes this file defines a hook of its own.

   Each stand-in records what the device asked of it, for the tests to read; the rest do nothing and say there is
   nothing (no frame, no screen size, the GPU running). A test resets the records it reads before each case. */
#include <stdlib.h>
#include <string.h>

/* hv_pvg_metal_create gives a Metal side (this flag's address) only when set */
static int metal_on;

/* Channel commands handed over, in order: channel and stamp, and the op word and length of each (a refused command
   comes as {0, 12}). while_handed, if set, runs once from inside the next hand-over. */
static uint32_t handed[64][2], nhanded;
static uint32_t handed_head[64][2];
static void (*while_handed)(void);
/* Deletes from the root ring: op and first argument (the task) of each, in order; forgot_after: how many channel
   commands were handed over before it */
static uint32_t forgot[8][2], forgot_after[8], nforgot;
/* The mapper: ids unmapped and fences asked for, in order; how many maps */
static uint32_t unmapped[64], nunmapped;
static uint64_t fences[64], fence_gens[64];
static uint32_t nfences;
static unsigned nmapped;
/* The mapper's state as last handed over whole (hv_pvg_metal_set_unmapped) */
static uint8_t handed_bits[HV_PVG_MAPPING_IDS / 8];
/* What the Metal side was told, in order: 'm' map, 'u' unmap, 'r' reset, 's' the state handed over, 'R' a restore */
static char told[64];
static unsigned ntold;
static void tell(char c) { if (ntold < sizeof told - 1) told[ntold++] = c, told[ntold] = 0; }
/* Called, if set, from the two hooks a CPU thread calls: hv_pvg_metal_cursor_moved (0) and
   hv_pvg_metal_mapper_reset (1) */
static void (*on_cpu_hook)(int which);

/* make test and runtests.sh set HV_NO_GPU and HV_PVG_NO_METAL so that no test reaches the Mac's GPU. The device
   checks them before it asks for its Metal side (pvg_lite.c ensure_metal), so they would keep this stand-in, which
   reaches no GPU, from being asked for too: they are cleared before main. A test of the switches sets them itself. */
__attribute__((constructor)) static void standin_no_switches(void) { unsetenv("HV_NO_GPU"), unsetenv("HV_PVG_NO_METAL"); }

HvPvgMetal *hv_pvg_metal_create(HvPvgLite *lite) { (void)lite; return metal_on ? (HvPvgMetal *)&metal_on : NULL; }
void hv_pvg_metal_submit(HvPvgMetal *m, HvPvgLite *lite, uint32_t ch, uint32_t *command, uint32_t length) {
    (void)m, (void)lite;
    if (nhanded < 64)
        handed_head[nhanded][0] = command[0], handed_head[nhanded][1] = length, handed[nhanded][0] = ch, handed[nhanded][1] = command[2], nhanded++;
    free(command);
    if (while_handed) { void (*f)(void) = while_handed; while_handed = NULL; f(); }
}
void hv_pvg_metal_forget(HvPvgMetal *m, uint32_t *command, uint32_t length) {
    (void)m;
    uint32_t skip = (command[0] >> 16) * 8;
    if (nforgot < 8)
        forgot_after[nforgot] = nhanded, forgot[nforgot][0] = command[0] & 0xffff,
        forgot[nforgot][1] = length >= 16 + skip ? command[3 + skip / 4] : ~0u, nforgot++;
    free(command);
}
void hv_pvg_metal_unmap(HvPvgMetal *m, uint32_t mapping) { (void)m; if (nunmapped < 64) unmapped[nunmapped++] = mapping; tell('u'); }
void hv_pvg_metal_map(HvPvgMetal *m, uint32_t mapping) { (void)m, (void)mapping; nmapped++; tell('m'); }
void hv_pvg_metal_mapper_reset(HvPvgMetal *m) { (void)m; tell('r'); if (on_cpu_hook) on_cpu_hook(1); }
void hv_pvg_metal_set_unmapped(HvPvgMetal *m, const uint8_t bits[HV_PVG_MAPPING_IDS / 8]) {
    (void)m;
    memcpy(handed_bits, bits, sizeof handed_bits);
    tell('s');
}
void hv_pvg_metal_mapper_fence(HvPvgMetal *m, uint64_t index, uint64_t gen) {
    (void)m;
    if (nfences < 64) fence_gens[nfences] = gen, fences[nfences++] = index;
}
void hv_pvg_metal_cursor_moved(HvPvgMetal *m) { (void)m; if (on_cpu_hook) on_cpu_hook(0); }
int hv_pvg_metal_snapshot(HvPvgMetal *m, uint8_t *bgra, uint32_t w, uint32_t h) { (void)m, (void)bgra, (void)w, (void)h; return -1; }
int hv_pvg_metal_frame(HvPvgMetal *m, uint8_t *to, size_t cap, uint32_t *w, uint32_t *h) { (void)m, (void)to, (void)cap, (void)w, (void)h; return -1; }
void hv_pvg_metal_stages(HvPvgMetal *m, uint32_t ms[HV_PVG_STAGES]) { (void)m; memset(ms, 0, HV_PVG_STAGES * sizeof *ms); }
void hv_pvg_metal_lab(HvPvgMetal *m, int readback, const char *when) { (void)m, (void)readback, (void)when; }
void hv_pvg_metal_hide_pointer(HvPvgMetal *m, int hide) { (void)m, (void)hide; }
void hv_pvg_metal_set_layer(HvPvgMetal *m, void *layer) { (void)m, (void)layer; }
uint64_t hv_pvg_metal_gpu_frames(HvPvgMetal *m) { (void)m; return 0; }
int hv_pvg_metal_gpu_faulted(HvPvgMetal *m) { (void)m; return 0; }
void hv_pvg_metal_gpu_rearm(HvPvgMetal *m) { (void)m; }
int hv_pvg_metal_gpu_state(HvPvgMetal *m) { (void)m; return HV_PVG_GPU_RUNS; }
int hv_pvg_metal_gpu_try_again(HvPvgMetal *m) { (void)m; return -1; }
int hv_pvg_metal_gpu_code_refused(HvPvgMetal *m) { (void)m; return 0; }
int hv_pvg_metal_hold(HvPvgMetal *m, uint64_t wait_ns) { (void)m, (void)wait_ns; return 0; }
int hv_pvg_metal_release(HvPvgMetal *m) { (void)m; return 0; }
int hv_pvg_metal_pointer_shown(HvPvgMetal *m) { (void)m; return 0; }
int hv_pvg_metal_screen_size(HvPvgMetal *m, uint32_t *w, uint32_t *h) { (void)m, (void)w, (void)h; return -1; }
int hv_pvg_metal_save(HvPvgMetal *m, const HvPvgSink *out, uint64_t deadline_ns, char *why, size_t why_len) {
    (void)m, (void)out, (void)deadline_ns, (void)why, (void)why_len;
    return 0; /* nothing of its own */
}
int hv_pvg_metal_check(const uint8_t *data, size_t len) { (void)data; return len == 0 ? 0 : -1; } /* what the stand-in saves */
void hv_pvg_metal_restore(HvPvgMetal *m, HvPvgBytes bytes) { (void)m; hv_pvg_bytes_release(&bytes); tell('R'); }
