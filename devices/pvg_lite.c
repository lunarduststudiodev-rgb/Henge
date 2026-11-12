/* The guest driver's view of the graphics device, worked out from its symbols
   and from tracing ParavirtualizedGraphics.framework (HV_PVG_TRACE=2):

   gfx registers
     +1000 enable, +1004 FIFO buffer size, +1010 ring offset in it (0x4000),
     +1030 FIFO buffer page (<< 14), +101c root page (<< 14)
     +1008 root ring write pointer (doorbell), +100c read pointer
     +1020 channel doorbell (the value is the channel)
     +1018 stamps-updated bits (1 << channel), +1014 display pokes (1 << port; both read and cleared), +102c faults
     +1034 protocol version (the driver offers one, the device answers),
     +122c number of displays

   FIFO buffer: a control area where stamp[channel] is at +4 * channel, then
   the root ring. Root page: channel records at 0x400 + 0x14 * (channel - 1)
   of {write, read, -1, id, page table page}; the table lists the ring's 16 KB
   pages as 32-bit page numbers, zero after the last.

   Every command, on every ring, starts {op, length, stamp}. It is complete
   when the device has stored its stamp and moved the read pointer.

   Display: channel 4 op 1 names a shared state page the device fills in
   (modes, seq at +200, ack pending at +fa9, state at +fc0); the guest reads
   +100 once it has set its mask at +104, acks with op 2. Swaps are op 8:
   {port, 0, u64 mapping id, u64 gamma offset, u64 gamma length, count, sum},
   and the device counts them at +f98 with the mapping id at +fb0 (both u64), and zeroes +fa4.

   A command with bit 16 of its op set first carries a barrier: {channel, stamp}.

   iosurface mapper: +1028 queue count, +1030 enable, +1000 ring, +1010
   descriptor table, +1008 size, +1018 submitted index, +1020 completed index.
   Ring entries are {op (1 map, 2 unmap, 3 test), mapping id, result, 0}. */

#include <unistd.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <os/lock.h>
#include <stdatomic.h>
#include "pvg_lite.h"
#include "frame_pace.h"
#include "gpu_switch.h"
#include "../host/host_fp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/machine.h"

#define MAX_CHANNELS 16

/* A task's tables, by the number in commands: its root page, and the task offsets of its object lists */
typedef struct PvgTasks { uint32_t root[512]; uint64_t objects[512], placement[512]; } PvgTasks;

struct HvPvgLite {
    HvMachine *m;
    HvGic *gic;
    int gfx_irq, iosfc_irq;
    int log;
    uint32_t regs[0x1000 / 4]; /* +1000.. */
    uint64_t io_regs[0x40 / 8];
    uint32_t root_rp, status, chan_pending;
    uint32_t poke;       /* display pokes for +1014, 1 << port: Henge's one display is port 0. Not saved (hv_pvg_lite_restored) */
    int port_told;       /* a display command for another port was logged */
    int root_pending, iosfc_pending;
    uint64_t display_pa;
    uint64_t display_logged; /* the page the modes were last logged for */
    uint64_t swaps;
    /* Diagnostic only (the poke, the display timing notes): the guest's mask
       word at display page +0x104 as last seen at a swap, how many times it was logged, and the swaps made while its
       bit 1 was set, when Apple's host also raises the display poke. Not saved. */
    uint32_t swap_mask, swap_mask_logs;
    uint64_t poke_swaps;
    /* The last stamp handed to the worker per channel, and who is being drained. */
    uint32_t submitted[MAX_CHANNELS];
    uint32_t draining;
    uint32_t waits_on[MAX_CHANNELS], wait_stamp[MAX_CHANNELS]; /* the channel and stamp each is waiting for (barrier_met), 0 none */
    _Atomic uint64_t wait_since[MAX_CHANNELS]; /* since when each (0: the root ring) has waited on a barrier, ns; 0 none */
    uint32_t cycles_told; /* channels said to wait, through others, on themselves: a bit each */
    int root_waiting; /* the root ring's next command waits for a channel's work (barrier_met) */
    uint64_t barriers, barrier_drains, barrier_retries, barrier_cycles;
    CFTypeRef layer; /* retained until replaced; layer_lock serializes lazy replay handoff */
    os_unfair_lock layer_lock;
    int layer_set; /* an explicit NULL also overrides the renderer's Mac test layer */
    int pointer_hidden; /* the host's choice, under layer_lock: kept for a renderer made later */
    /* Each task's tables twice: as this side reads the commands (saved; its HV_PVG_TALLY logs read task memory by
       them), and as the Metal side's worker has reached them, which every read the worker makes uses. The device reads
       a define or an object list ahead of the worker, which may still have the number's old task's work queued. */
    PvgTasks tasks, worker_tasks;
    int old_driver; /* macOS 14's (and older) GPU driver: object lists at a fixed place, compact surface descriptions */
    uint32_t root_told[8]; /* root ring ops below 0x100 already logged as seen, a bit each */
    uint32_t long_told[8]; /* root ring ops already logged as too long to take, a bit each */
    int mapping_told; /* a mapper id outside what Apple's host takes, logged */
    uint32_t barriers_told; /* rings with a command refused for its barriers, logged: bit 0 the root ring, else the channel's */
    uint32_t bad_seen[6]; /* refusals a guest can repeat at every doorbell (enum pvg_bad), counted: logged at 1, 10, 100... */
    uint8_t obj_seen[512][8192];
    int tally;
    uint64_t counts[6][256];
    uint64_t execs, bad;
    double last_tally;
    _Atomic(HvPvgMetal *) metal; /* release-published after the cached layer is applied */
    int metal_tried;
    uint32_t stamps_pending; /* channels Metal finished, for the next poll */
    uint64_t vblanks;        /* refresh events sent (HV_PVG_VBLANK) */
    int vblank_on;           /* enabled via HV_PVG_VBLANK or hv_pvg_lite_set_vblank */
    /* The mapper: io_regs[0x20 / 8] (completed) is read and written atomically, for the GPU's completion
       publishes it. mapper_rp: the next entry to walk; mapper_fenced: the index the last fence will complete;
       mapper_gen: bumped whenever the guest restarts the ring (or a snapshot loads), so a fence asked for
       before then completes nothing. None of these is saved: they are derived on load. */
    uint64_t mapper_rp, mapper_fenced;
    _Atomic uint64_t mapper_gen;
    /* A walk left for the next poll (do_mapper's HV_PVG_MAPPER_BATCH), and whether what it walked so far unmapped any */
    int mapper_more, mapper_unmapped;
    /* The ids the guest unmapped and has not mapped again, a bit each (pvg_lite.h's hv_pvg_metal_set_unmapped): their
       descriptors may name pages it has since reused. Kept here, where every mapper op is seen (those before the Metal
       side exists too), and saved (PVGS). Whenever the walk moves past entries it did not read, they may have mapped
       any id again: every bit is cleared (unknown), never kept on a guess (mapper_unknown). Under the device lock. */
    uint8_t unmapped[HV_PVG_MAPPING_IDS / 8];
    /* Suspend: the Metal part of a loaded snapshot, for hv_pvg_lite_restored */
    HvPvgBytes restore_metal; /* the GPU's saved part, from the load to the restore */
};

/* What the guest's GetDeviceInfo asks for: key, value pairs, as the framework
   answers on an Apple GPU. */
static const uint32_t device_info[][2] = {
    {0x01, 4},      {0x02, 0},     {0x03, 0x400},  {0x04, 0x400},  {0x05, 0x400},  {0x06, 0x8000}, {0x07, 1},
    {0x08, 1},      {0x09, 1},     {0x0a, 8},      {0x0b, 0x1ff},  {0x0c, 1},      {0x0d, 0x10},   {0x0e, 1},
    {0x0f, 0x100},  {0x10, 1},     {0x11, 1},      {0x12, 0x20007}, {0x13, 1},     {0x14, 2},      {0x15, 1},
    {0x16, 2},      {0x17, 1},     {0x18, 1},      {0x19, 1},      {0x1a, 1},      {0x1b, 1},      {0x1c, 7},
    {0x21, 0xfff},  {0x24, 0xffffffff}, {0x1d, 0x400}, {0x1e, 0x1f}, {0x1f, 0x10000}, {0x20, 0x10}, {0x22, 5},
    {0x29, 7},      {0x23, 0x800}, {0x25, 0x3f1},  {0x28, 0x10},
};

static const uint32_t *guest32(HvPvgLite *p, uint64_t pa) { return hv_phys_ptr_r(p->m, pa, 4); }
/* A word the device will store to: an auto-save's guard keeps the page as it was first. */
static uint32_t *guest32_w(HvPvgLite *p, uint64_t pa) { return hv_phys_ptr_w(p->m, pa, 4); }

static uint32_t reg(HvPvgLite *p, uint32_t off) { return p->regs[(off - 0x1000) / 4]; }

static void do_channel(HvPvgLite *p, uint32_t ch);

/* A command's barrier {channel, stamp}, on channel self (0: the root ring): met once that channel's work up to the stamp
   is handed to the worker, which drains it here if the guest has written it. Until the guest has, the command waits
   (its ring is left there) for a later doorbell or poll to find it written, however long that is: the barrier is an
   order the guest asked for, and work let past it would read results not made yet. A channel that waits, through
   others or not, on one that waits on it can never go on; that is said once for each, and it waits on as the guest
   asked. Nothing waits on the root ring, so it is never in such a loop, but it can wait behind one. */
static int barrier_met(HvPvgLite *p, uint32_t self, uint32_t ch, uint32_t stamp) {
    if (!ch || ch >= MAX_CHANNELS || ch == self) return 1;
    p->barriers++;
    if ((int32_t)(p->submitted[ch] - stamp) < 0) {
        /* Said before the drain: a walk of ch that comes back to self's own wait finds it (and that ch's own wait,
           made in the drain, closes a loop through self), not on a later poll */
        if (self) p->waits_on[self] = ch, p->wait_stamp[self] = stamp;
        p->barrier_drains++;
        do_channel(p, ch);
    }
    if ((int32_t)(p->submitted[ch] - stamp) >= 0) return 1; /* a wait said above is met, and the walk passes it */
    p->barrier_retries++;
    /* The chain of waits ahead of this one. A channel counts only while what it waits for is still not handed over:
       one further up this walk (a drain is a walk inside a walk) still names the channel it waited on before, which
       the work handed over since may already have met. */
    uint32_t c = ch;
    for (int i = 0; i < MAX_CHANNELS && c; i++) {
        if (c == self) {
            p->barrier_cycles++;
            if (!(p->cycles_told & (1u << self))) {
                p->cycles_told |= 1u << self;
                fprintf(stderr, "hengevm: pvg-lite: channel %u waits for channel %u stamp %x, which waits for channel %u: neither can go on\n",
                        self, ch, stamp, self);
            }
            break;
        }
        uint32_t next = p->waits_on[c];
        c = next && (int32_t)(p->submitted[next] - p->wait_stamp[c]) < 0 ? next : 0;
    }
    /* c: back at self, or a chain longer than the channels (a loop of others ahead of it): the wait can't end */
    int endless = c != 0;
    /* For the freeze recorder: since when this has waited. A wait that can't end is told above, once; as a graphics
       stage it would be held up through every stall from then on, whatever held each of them up. */
    if (endless) atomic_store_explicit(&p->wait_since[self], 0, memory_order_relaxed);
    else if (!atomic_load_explicit(&p->wait_since[self], memory_order_relaxed))
        atomic_store_explicit(&p->wait_since[self], clock_gettime_nsec_np(CLOCK_UPTIME_RAW), memory_order_relaxed);
    return 0;
}

/* A define (0x38) {id << 1, size, size high, root page} or an object list (0x44) {id, objects, placement, their
   lengths} into a set of task tables. A define starts its task empty: Apple's host
   (-[_PGDevice createTaskID:taskRoot:length:restoring:]) refuses a number already in use ("Host task already exists"),
   so a guest only defines a number that is free, and no list kept under that number belongs to the new task. */
static void tasks_apply(PvgTasks *t, uint32_t op, const uint32_t *args, uint32_t n) {
    if (op == 0x38 && n >= 16 && args[0] >> 1 < 512) {
        uint32_t id = args[0] >> 1;
        t->root[id] = args[3];
        t->objects[id] = t->placement[id] = 0;
    } else if (op == 0x44 && n >= 28 && args[0] < 512) {
        t->objects[args[0]] = (uint64_t)args[2] << 32 | args[1];
        t->placement[args[0]] = (uint64_t)args[4] << 32 | args[3];
    }
}

/* Read by this side (the root ring or a channel): its own tables, and what it logged of the number's objects */
static void task_defined(HvPvgLite *p, const uint32_t args[4]) {
    tasks_apply(&p->tasks, 0x38, args, 16);
    if (args[0] >> 1 < 512) memset(p->obj_seen[args[0] >> 1], 0, sizeof p->obj_seen[args[0] >> 1]);
}

/* The worker reached a command: a define or object list is the worker's from now on, and not before (its work for the
   number's old task, queued ahead, reads the old tables) */
void hv_pvg_lite_task_command(HvPvgLite *p, uint32_t op, const uint32_t *args, uint32_t n) {
    tasks_apply(&p->worker_tasks, op, args, n);
}

/* 1 the first time op (below 0x100) is asked about, a bit each */
/* What a guest can get refused again at every doorbell write, each counted on its own and logged at the 1st, 10th,
   100th... time: a guest that writes one bad header and rings in a loop would otherwise be a line a write. */
enum pvg_bad { PVG_BAD_ROOT_WP, PVG_BAD_ROOT_CMD, PVG_BAD_CHAN_CMD, PVG_BAD_MAPPER_WANT, PVG_BAD_MAPPER_RING, PVG_BAD_DISPLAY };
static uint32_t bad_count(HvPvgLite *p, enum pvg_bad k) {
    /* atomic: a display command is refused on the Metal worker, the rest on the device's thread */
    uint32_t n = __atomic_add_fetch(&p->bad_seen[k], 1, __ATOMIC_RELAXED), m = n;
    while (m && m % 10 == 0) m /= 10; /* 0 once the count wraps: not a power of ten */
    return m == 1 ? n : 0;
}

static int told_once(uint32_t told[8], uint32_t op) {
    uint32_t bit = 1u << (op % 32);
    if (told[op / 32] & bit) return 0;
    told[op / 32] |= bit;
    return 1;
}

/* The root ring's other commands. Apple's host takes every command on either ring (PGRootFIFO keeps PGFIFO's
   processFifo, and no command handler asks which ring it came on), so a delete may come here: it goes to the Metal
   side's worker, behind the channel work handed to it before. Nothing is made before the first channel command
   (ensure_metal), so with no Metal side there is nothing to forget. Every other op is logged the first time it is
   seen, saying whether it was taken, for the ring macOS uses for each is not known from here. 0 if one to be taken
   could not be copied (no memory): it is left on the ring, and nothing of it applied here either, for the next poll. */
#define ROOT_HANDED_MAX 0x1000 /* the longest root command copied for the worker: each is a few words after its barriers */
static int root_other(HvPvgLite *p, uint64_t ring_pa, uint32_t ring, uint32_t at, const uint32_t *hdr) {
    uint32_t op = hdr[0] & 0xffff;
    if (op == 0x3a) return 1;
    /* The deletes, and a define, which forgets what the Metal side kept under that number (task_defined) */
    int forgets = op == 0x20 || op == 0x25 || op == 0x28 || op == 0x38;
    HvPvgMetal *metal = atomic_load_explicit(&p->metal, memory_order_acquire);
    int too_long = forgets && hdr[1] > ROOT_HANDED_MAX, taken = forgets && metal && !too_long;
    int seen = op != 0x38 && op < 0x100 && told_once(p->root_told, op);
    if (seen)
        fprintf(stderr, "hengevm: pvg-lite: root ring op %x seen: %s\n", op,
                taken ? "taken" : !forgets ? "not taken" : too_long ? "too long, not taken" : "nothing to forget yet");
    /* Once an op: a guest could send it on every command. A define too long isn't taken by the device's tables either
       (root_walk), so the two copies still agree. */
    if (too_long && told_once(p->long_told, op) && !seen)
        fprintf(stderr, "hengevm: pvg-lite: root ring op %x of %u bytes, longer than any macOS sends: not taken\n", op, hdr[1]);
    if (!taken) return 1;
    uint32_t words = hdr[1] / 4, *cmd = malloc(words * 4);
    if (!cmd) return 0;
    for (uint32_t i = 0; i < words; i++) {
        const uint32_t *w = guest32(p, ring_pa + (at + 4u * i) % ring);
        cmd[i] = w ? *w : 0;
    }
    hv_pvg_metal_forget(metal, cmd, words * 4);
    return 1;
}

/* A command is checked as Apple's host checks it (-[PGFIFO processFifo]), on the root ring as on a channel. First its
   length: its 12-byte header at least, and no more than was written. Apple's host takes nothing of one that isn't (it
   logs it and pushes no stamp), and where the next command starts can't be known: the rest of what was written is
   skipped, and its stamps are reached only by work the guest writes after it. */
static int command_length_ok(const uint32_t hdr[3], uint32_t written) {
    return hdr[1] >= 12 && hdr[1] <= written;
}

/* Then its {channel, stamp} barriers (bits 31..16): at most 64, and all within its length. Read past its end, a claimed
   barrier would be the next command's bytes, and a stamp in them never reached would hold the ring for good. Apple's
   host refuses a command that fails either, and pushes and faults its stamp (pushStamp:cmdID:, faultAtStampValue:),
   as for an op it doesn't know: the command is not run, but its stamp is reached, in order, so what waits on it goes
   on. Its length is good, so the walk goes on after it. */
#define PVG_BARRIERS_MAX 64
static int command_barriers_ok(const uint32_t hdr[3]) {
    uint32_t k = hdr[0] >> 16;
    return k <= PVG_BARRIERS_MAX && hdr[1] >= 12 + k * 8;
}

static void root_walk(HvPvgLite *p);
/* Nothing on the root ring waits once it is walked without a command left waiting, whichever way that was
   (a bad length or write index skips what waited) */
static void do_root(HvPvgLite *p) {
    root_walk(p);
    if (!p->root_waiting) atomic_store_explicit(&p->wait_since[0], 0, memory_order_relaxed);
}

static void root_walk(HvPvgLite *p) {
    p->root_waiting = 0; /* until a command is found waiting */
    uint64_t buf = (uint64_t)reg(p, 0x1030) << 14;
    uint32_t off = reg(p, 0x1010), size = reg(p, 0x1004), wp = reg(p, 0x1008);
    if (!buf || size <= off) return;
    uint32_t ring = size - off;
    /* The guest owns wp: one more than a ring ahead of what was read means it is
       not a ring position any more, and walking it would never end. */
    if (wp - p->root_rp > ring) {
        uint32_t n = bad_count(p, PVG_BAD_ROOT_WP);
        if (n) fprintf(stderr, "hengevm: pvg-lite: root ring write %x is more than a ring past %x (%u so far)\n", wp, p->root_rp, n);
        p->root_rp = wp;
        return;
    }
    while (p->root_rp != wp) {
        uint32_t at = p->root_rp % ring, hdr[3];
        for (int i = 0; i < 3; i++) {
            const uint32_t *w = guest32(p, buf + off + (at + 4u * i) % ring);
            hdr[i] = w ? *w : 0;
        }
        /* Every step moves at least 12 bytes and never past what was written. */
        if (!command_length_ok(hdr, wp - p->root_rp)) {
            uint32_t n = bad_count(p, PVG_BAD_ROOT_CMD);
            if (n) fprintf(stderr, "hengevm: pvg-lite: bad root command %x length %x (%u so far)\n", hdr[0], hdr[1], n);
            p->root_rp = wp;
            break;
        }
        if (!command_barriers_ok(hdr)) { /* not run; its stamp reached */
            if (!(p->barriers_told & 1u)) fprintf(stderr, "hengevm: pvg-lite: root command %x of length %x: its barriers don't fit, refused\n", hdr[0], hdr[1]);
            p->barriers_told |= 1u;
            p->root_rp += hdr[1];
            uint32_t *stamp = guest32_w(p, buf);
            if (stamp) __atomic_store_n(stamp, hdr[2], __ATOMIC_RELEASE), hv_dma_written(p->m, buf, 4);
            continue;
        }
        if (p->log) fprintf(stderr, "hengevm: pvg-lite: root op %x len %x stamp %x\n", hdr[0], hdr[1], hdr[2]);
        /* Its barriers first, as on a channel: a delete must not overtake the work it follows. Also before the Metal side
           is made (by the first channel command, which the barrier's drain may be): the work it names, handed over
           after a delete that went first, would make objects for a task already deleted. Only with none to be had
           (ensure_metal tried and failed) is there no worker to keep in order. */
        int blocked = 0, ordered = p->metal || !p->metal_tried;
        for (uint32_t k = 0; ordered && k < hdr[0] >> 16 && !blocked; k++) {
            const uint32_t *b0 = guest32(p, buf + off + (at + 12 + 8 * k) % ring), *b1 = guest32(p, buf + off + (at + 16 + 8 * k) % ring);
            blocked = !barrier_met(p, 0, b0 ? *b0 : 0, b1 ? *b1 : 0);
        }
        p->root_waiting = blocked;
        if (blocked) break;
        atomic_store_explicit(&p->wait_since[0], 0, memory_order_relaxed);
        /* Handed to the worker first: one that can't be is left on the ring, none of it done here, for the next poll */
        if (!root_other(p, buf + off, ring, at, hdr)) {
            p->root_waiting = 1;
            break;
        }
        uint32_t a = 12 + (hdr[0] >> 16) * 8; /* the arguments, after the barriers */
        /* a define the worker is handed too (root_other), or none is there yet: the two copies of the tables agree */
        if ((hdr[0] & 0xffff) == 0x38 && hdr[1] >= a + 16 && hdr[1] <= ROOT_HANDED_MAX) {
            const uint32_t *id = guest32(p, buf + off + (at + a) % ring), *page = guest32(p, buf + off + (at + a + 12) % ring);
            if (id && page) task_defined(p, (uint32_t[4]){*id, 0, 0, *page});
        }
        if ((hdr[0] & 0xffff) == 0x3a && hdr[1] >= a + 12) {
            const uint32_t *count = guest32(p, buf + off + (at + a) % ring), *page = guest32(p, buf + off + (at + a + 8) % ring);
            uint64_t to = page ? (uint64_t)*page << 14 : 0; /* read once: the page lent is the page marked */
            uint32_t *dst = page ? hv_phys_ptr_w(p->m, to, sizeof device_info) : NULL;
            if (dst && count) {
                uint32_t n = sizeof device_info / sizeof device_info[0];
                memcpy(dst, device_info, sizeof device_info);
                hv_dma_written(p->m, to, sizeof device_info);
                if (p->log) fprintf(stderr, "hengevm: pvg-lite: device info: %u keys (asked %u)\n", n, *count);
            }
        }
        p->root_rp += hdr[1];
        uint32_t *stamp = guest32_w(p, buf);
        if (stamp) __atomic_store_n(stamp, hdr[2], __ATOMIC_RELEASE), hv_dma_written(p->m, buf, 4);
    }
}

/* Reads a command's bytes out of a ring made of scattered pages. */
static void ring_read(HvPvgLite *p, const uint32_t *table, uint32_t size, uint32_t at, void *to, uint32_t n) {
    uint8_t *d = to;
    while (n) {
        uint32_t o = at % size, c = 0x4000 - (o & 0x3fff);
        if (c > n) c = n;
        const uint8_t *q = hv_phys_ptr_r(p->m, ((uint64_t)table[o >> 14] << 14) + (o & 0x3fff), c);
        if (q) memcpy(d, q, c);
        else memset(d, 0, c);
        d += c, at += c, n -= c;
    }
}

/* The panel's own size: the first mode the display offers, and the one macOS starts in. */
static uint32_t display_w = 1920, display_h = 1080;
static int display_set; /* the app (or --display) has named it */
/* The panel's real size, when known: macOS works out the pixel density from it and
   picks HiDPI (2x) for a Retina-dense panel, as on a real Mac. 0 = the old 400 mm. */
static uint32_t display_mm_w, display_mm_h;

void hv_pvg_lite_set_size_mm(uint32_t width_mm, uint32_t height_mm) {
    display_mm_w = width_mm;
    display_mm_h = height_mm;
}

void hv_pvg_lite_set_mode(uint32_t width, uint32_t height) {
    display_w = width;
    display_h = height;
    display_set = 1;
}

/* What macOS starts in, before it has shown anything: the panel's own mode, drawn at 2 pixels a
   point on a Retina-dense panel (the density display_modes judges by). -1 until the app has set it. */
int hv_pvg_lite_start_mode(uint32_t *width, uint32_t *height, uint32_t *scale) {
    if (!display_set) return -1;
    uint32_t width_mm = display_mm_w && display_mm_h ? display_mm_w : 400;
    *width = display_w, *height = display_h;
    *scale = display_w / (width_mm / 25.4) >= 200 ? 2 : 1;
    return 0;
}

/* The sizes macOS may use. A Retina-dense panel (as an iPad's is, once its real size is known) gets
   what a Mac's built-in display offers in Displays settings, from Larger Text to More Space: macOS
   draws each at 2x, so each is a mode twice the size it looks, around the panel's own, which is
   first. The presenter fits whichever macOS picks to the screen. Otherwise just the one size. */
enum { DISPLAY_MODES_MAX = 8 };
/* What the guest was last offered: modes[0] is the panel's own. */
static uint32_t offered[DISPLAY_MODES_MAX][2], offered_n;
static int offered_dense;

static uint32_t display_modes(uint32_t width_mm, uint32_t out[][2]) {
    out[0][0] = display_w, out[0][1] = display_h;
    double ppi = width_mm ? display_w / (width_mm / 25.4) : 0;
    offered_dense = ppi >= 200;
    if (ppi < 200 || display_w < 1600 || display_h < 1000) return 1;
    /* How big things look against the panel's own 2x: 0.75 is larger text, 1.25 more space. */
    static const double looks[] = {0.75, 0.875, 1.125, 1.25};
    uint32_t n = 1;
    for (size_t i = 0; i < sizeof looks / sizeof looks[0] && n < DISPLAY_MODES_MAX; i++) {
        uint32_t w = (uint32_t)(display_w * looks[i] / 8 + 0.5) * 8;                    /* looks like w / 2, a multiple of 4 */
        uint32_t h = (uint32_t)((double)w * display_h / display_w / 2 + 0.5) * 2;       /* the panel's shape, even */
        if (w > 3584 || (uint64_t)w * h > 9u << 20) continue; /* the guest's GPU memory and the presenter's work */
        out[n][0] = w, out[n][1] = h, n++;
    }
    return n;
}

/* The display poke (Apple's -[_PGDevice displayPokePort:]): the port's bit for +1014, and the interrupt a stamp
   raises. The guest asks for it with its mask word at display page +104. */
static void display_poke(HvPvgLite *p) {
    __atomic_fetch_or(&p->poke, 1u, __ATOMIC_ACQ_REL);
    hv_gicv3_pend(p->gic, p->gfx_irq);
    if (p->m->ncpus == 1) hv_machine_wake(p->m);
}

/* +122c says one display, so port 0 is the only one (display_command) */
static int display_port(HvPvgLite *p, uint32_t port) {
    if (port == 0) return 1;
    if (!p->port_told) {
        p->port_told = 1;
        fprintf(stderr, "hengevm: display: a command for port %u, past the one display: not taken\n", port);
    }
    return 0;
}

static void display_set_page(HvPvgLite *p, uint64_t pa) {
    uint8_t *d = hv_phys_ptr_w(p->m, pa, 0x4000);
    if (!d) return;
    p->display_pa = pa;
#define PUT32(off, v) (*(uint32_t *)(d + (off)) = (uint32_t)(v))
#define PUTF(off, v) (*(float *)(d + (off)) = (v))
    PUT32(0x00, 1);
    uint32_t width_mm = 400, height_mm = 400 * display_h / display_w; /* the same pixel density at any shape */
    if (display_mm_w && display_mm_h) width_mm = display_mm_w, height_mm = display_mm_h;
    PUT32(0x14, height_mm << 16 | width_mm);
    PUT32(0x18, 0x01000100);
    PUT32(0x1c, 0x400);
    PUT32(0x20, 3);
    PUTF(0x24, (float)width_mm);
    PUTF(0x28, (float)height_mm);
    {   /* The modes: 16 bytes each from 0x210, u16 width, u16 height, u32 refresh (16.16), flags
           (bits 0 and 1 together: HDR), the rest zero; their count at 0x208. The first is the panel's
           own (NativeFormat...Pixels). macOS takes a mode as it is: switching asks nothing of us,
           the surfaces it sends just change size. m4 fork: 120 Hz on ProMotion (HV_HZ). */
        const char *hz = getenv("HV_HZ");
        uint32_t r = hz && atoi(hz) > 0 ? (uint32_t)atoi(hz) : 60;
        uint32_t modes[DISPLAY_MODES_MAX][2];
        uint32_t n = display_modes(width_mm, modes);
        memcpy(offered, modes, sizeof modes);
        offered_n = n;
        memset(d + 0x210, 0, 16 * DISPLAY_MODES_MAX);
        PUT32(0x208, n);
        for (uint32_t i = 0; i < n; i++) {
            PUT32(0x210 + 16 * i, modes[i][1] << 16 | modes[i][0]);
            PUT32(0x214 + 16 * i, r << 16);
        }
        if (p->display_logged != pa) {
            p->display_logged = pa;
            fprintf(stderr, "hengevm: display: %u mode%s at %u Hz, %ux%u mm:", n, n == 1 ? "" : "s", r, width_mm, height_mm);
            for (uint32_t i = 0; i < n; i++) fprintf(stderr, " %ux%u", modes[i][0], modes[i][1]);
            fprintf(stderr, "\n");
        }
    }
    PUTF(0xa10, 1.0f); /* identity colour matrix */
    PUTF(0xa24, 1.0f);
    PUTF(0xa38, 1.0f);
    d[0xa73] = 1;
    PUT32(0x200, 1); /* the mode list's sequence number, which the guest acks */
    d[0xfa9] = 1;    /* ack pending */
    PUT32(0xfc0, 1); /* state: told the guest, waiting for the ack */
    /* The modes posted: or'd in, as Apple's state machine does, and a poke if the guest's mask asks for it */
    __atomic_fetch_or((uint32_t *)(d + 0x100), 4, __ATOMIC_SEQ_CST); /* the flag, then the mask: see display_swap */
    hv_dma_written(p->m, pa, 0x4000);
    if (__atomic_load_n((const uint32_t *)(d + 0x104), __ATOMIC_SEQ_CST) & 4) display_poke(p);
#undef PUT32
#undef PUTF
}

static void display_ack(HvPvgLite *p, uint32_t seq) {
    uint8_t *d = p->display_pa ? hv_phys_ptr_w(p->m, p->display_pa, 0x4000) : NULL;
    if (!d) return;
    *(uint32_t *)(d + 0xfbc) = seq;
    d[0xfa9] = 0;
    *(uint32_t *)(d + 0xfc0) = 2;
    hv_dma_written(p->m, p->display_pa, 0x4000);
}

/* HV_PVG_TRACE_FILE names a file that exists: looked for at most once a second, so it can still be made or removed
   while the Mac runs (the iPad app always names Documents/trace-on), with no file-system call per command. */
int hv_pvg_tracing(void) {
    static _Atomic uint64_t looked; /* ns, monotonic; 0: never */
    static _Atomic int on;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), then = atomic_load_explicit(&looked, memory_order_relaxed);
    if ((!then || (int64_t)(now - then) >= 1000000000) && atomic_compare_exchange_strong(&looked, &then, now ? now : 1)) {
        const char *f = getenv("HV_PVG_TRACE_FILE");
        atomic_store_explicit(&on, f && access(f, F_OK) == 0, memory_order_relaxed);
    }
    return atomic_load_explicit(&on, memory_order_relaxed);
}

static void display_swap(HvPvgLite *p, uint64_t mapping, const uint32_t *args) {
    uint8_t *d = p->display_pa ? hv_phys_ptr_w(p->m, p->display_pa, 0x4000) : NULL;
    if (!d) return;
    memcpy(d + 0xf80, args + 4, 16); /* gamma table offset, length */
    memcpy(d + 0xf90, args + 8, 8);  /* gamma entries, sum */
    p->swaps++;
    /* As Apple's _presentMappedSurface: the page's own 64-bit count goes up one, the 64-bit mapping, and +fa4 zero */
    *(uint64_t *)(d + 0xf98) += 1;
    *(uint64_t *)(d + 0xfb0) = mapping;
    *(uint32_t *)(d + 0xfa4) = 0;
    /* Sequentially consistent, as Apple's ldsetal and ldar: the guest sets its mask, then reads the flag, and waits
       for a poke if it isn't set. With release and acquire (ldsetl, ldapr) this mask read could come before the flag
       is seen, and both sides would miss: the guest no flag, the host no mask, and no poke. */
    __atomic_fetch_or((uint32_t *)(d + 0x100), 2, __ATOMIC_SEQ_CST);
    hv_dma_written(p->m, p->display_pa, 0x4000);
    /* Apple's host reads this word right after setting the flag and pokes the guest when bit 1 is set. Logged when it changes (16 times at most: it is the guest's to write) and as the swaps that asked reach
       each power of two, so a log says whether macOS asks. */
    uint32_t mask = __atomic_load_n((const uint32_t *)(d + 0x104), __ATOMIC_SEQ_CST);
    int asked = 0;
    if (mask & 2) {
        uint64_t n = ++p->poke_swaps;
        asked = !(n & (n - 1));
    }
    if ((mask != p->swap_mask && p->swap_mask_logs < 16) || asked) {
        if (mask != p->swap_mask) p->swap_mask_logs++;
        fprintf(stderr, "hengevm: display: guest mask word %x (was %x) at swap %llu; %llu swaps asked for the poke\n", mask,
                p->swap_mask, (unsigned long long)p->swaps, (unsigned long long)p->poke_swaps);
    }
    p->swap_mask = mask;
    /* After the flag, in the order Apple's _presentMappedSurface gives them. Henge does all of this when the swap command
       is read, not once its frame is on the screen. */
    if (mask & 2) display_poke(p);
    if (p->log) fprintf(stderr, "hengevm: pvg-lite: swap %llu: mapping %llu\n", (unsigned long long)p->swaps, (unsigned long long)mapping);
}

/* The fewest argument bytes each display command takes, as Apple's PGFIFO handlers check them: a shorter one is
   logged with its payload and the host stops. 0: not a display command. */
static uint32_t display_least(uint32_t op) {
    switch (op) {
    case 1: case 2: case 5: return 8; /* set page, ack, cursor show (CmdDisplayCursorShow) */
    case 4: return 0x2c;              /* cursor glyph (CmdDisplayCursorGlyph): to the word at +28 */
    case 8: return 0x28;              /* swap (CmdDisplaySwapMapping): the ten words display_swap reads */
    default: return 0;
    }
}

/* A display command (1 set page, 2 ack, 4 cursor glyph, 5 cursor show, 8 swap; the first word its port): 1 taken,
   0 not, for one shorter than Apple's host takes, a glyph or swap whose second word isn't zero (Apple's host stops there too)
   or a port past the one display. Apple's host has no display there either (it throws for a set-page, asserts for
   the others). The cursor's own work is the Metal side's (cursorGlyph, cursor_shown), only for a command taken. */
static int display_command(HvPvgLite *p, uint32_t op, const uint32_t *args, uint32_t n) {
    if (n < display_least(op) || ((op == 4 || op == 8) && args[1])) {
        uint32_t seen = bad_count(p, PVG_BAD_DISPLAY);
        if (seen) fprintf(stderr, "hengevm: display: command %u with %u bytes of arguments, word 1 %x: not taken (%u so far)\n", op, n, args[1], seen);
        return 0;
    }
    if (!display_port(p, args[0])) return 0;
    if (op == 1) display_set_page(p, (uint64_t)args[1] << 14);
    else if (op == 2) display_ack(p, args[1]);
    else if (op == 8) display_swap(p, (uint64_t)args[3] << 32 | args[2], args);
    return 1;
}

/* Task memory: the root page holds {table page, depth}; each level has 4096
   32-bit page numbers, so one level maps 64 MB. Returns a host pointer valid
   to the end of the guest's 16 KB page. */
static const uint8_t *task_ptr(HvPvgLite *p, const PvgTasks *t, uint32_t task, uint64_t off, uint32_t *avail) {
    if (task >= 512 || !t->root[task]) return NULL;
    const uint32_t *root = hv_phys_ptr_r(p->m, (uint64_t)t->root[task] << 14, 0x4000);
    if (!root) return NULL;
    uint32_t page = root[0], depth = root[1];
    if (depth < 1 || depth > 3) return NULL;
    uint64_t vpn = off >> 14;
    for (int level = (int)depth - 1; level >= 0; level--) {
        const uint32_t *t = page ? hv_phys_ptr_r(p->m, (uint64_t)page << 14, 0x4000) : NULL;
        if (!t) return NULL;
        page = t[(vpn >> (12 * level)) & 0xfff] & 0x7fffffff; /* bit 31 is a flag */
    }
    if (!page) return NULL;
    uint32_t in = (uint32_t)(off & 0x3fff);
    if (avail) *avail = 0x4000 - in;
    return hv_phys_ptr_r(p->m, ((uint64_t)page << 14) + in, 0x4000 - in);
}

static int task_read(HvPvgLite *p, const PvgTasks *t, uint32_t task, uint64_t off, void *to, uint32_t n) {
    uint8_t *d = to;
    while (n) {
        uint32_t avail = 0;
        const uint8_t *q = task_ptr(p, t, task, off, &avail);
        if (!q) return -1;
        uint32_t c = n < avail ? n : avail;
        memcpy(d, q, c);
        d += c, off += c, n -= c;
    }
    return 0;
}

int hv_pvg_lite_task_read(HvPvgLite *p, uint32_t task, uint64_t offset, void *to, uint32_t length) {
    return task_read(p, &p->worker_tasks, task, offset, to, length);
}

uint32_t hv_pvg_lite_task_page(HvPvgLite *p, uint32_t task, uint64_t offset) {
    if (task >= 512 || !p->worker_tasks.root[task]) return 0;
    const uint32_t *root = hv_phys_ptr_r(p->m, (uint64_t)p->worker_tasks.root[task] << 14, 0x4000);
    if (!root) return 0;
    uint32_t page = root[0], depth = root[1];
    if (depth < 1 || depth > 3) return 0;
    uint64_t vpn = offset >> 14;
    for (int level = (int)depth - 1; level >= 0; level--) {
        const uint32_t *t = page ? hv_phys_ptr_r(p->m, (uint64_t)page << 14, 0x4000) : NULL;
        if (!t) return 0;
        page = t[(vpn >> (12 * level)) & 0xfff] & 0x7fffffff;
    }
    return page;
}

/* n pages' numbers from a task page on, 0 for those not mapped: a table at a time, for it is asked often. */
void hv_pvg_lite_task_pages(HvPvgLite *p, uint32_t task, uint64_t first, uint64_t n, uint32_t *out) {
    memset(out, 0, n * 4);
    if (task >= 512 || !p->worker_tasks.root[task]) return;
    const uint32_t *root = hv_phys_ptr_r(p->m, (uint64_t)p->worker_tasks.root[task] << 14, 0x4000);
    if (!root) return;
    uint32_t top = root[0], depth = root[1];
    if (depth < 1 || depth > 3) return;
    for (uint64_t i = 0; i < n;) {
        uint64_t vpn = first + i;
        uint32_t page = top;
        for (int level = (int)depth - 1; level >= 1 && page; level--) {
            const uint32_t *t = hv_phys_ptr_r(p->m, (uint64_t)page << 14, 0x4000);
            page = t ? t[(vpn >> (12 * level)) & 0xfff] & 0x7fffffff : 0;
        }
        const uint32_t *leaf = page ? hv_phys_ptr_r(p->m, (uint64_t)page << 14, 0x4000) : NULL;
        uint64_t in = vpn & 0xfff, run = 4096 - in;
        if (run > n - i) run = n - i;
        if (leaf)
            for (uint64_t k = 0; k < run; k++) out[i + k] = leaf[in + k] & 0x7fffffff;
        i += run;
    }
}

/* For the GPU's mappings (and reads): what the GPU writes through a mapping is kept by the mapping itself
   (hv_ramfile_gpu_map claims and tracks its pages), so these lend no page; a C write through one would need
   hv_phys_ptr_w and hv_dma_written. */
uint8_t *hv_pvg_lite_page(HvPvgLite *p, uint32_t guest_page) {
    return (uint8_t *)(uintptr_t)hv_phys_ptr_r(p->m, (uint64_t)guest_page << 14, 0x4000);
}

void *hv_pvg_lite_phys(HvPvgLite *p, uint64_t pa, uint64_t length) {
    return (void *)(uintptr_t)hv_phys_ptr_r(p->m, pa, length);
}

/* macOS 15 and later name each task's object list with command 0x44. Older guest drivers (macOS 13/14
   seen) never send it: their list is at task offset 0x4000, the same 12-byte entries. */
#define OLD_DRIVER_OBJECTS 0x4000
uint64_t hv_pvg_lite_task_objects(HvPvgLite *p, uint32_t task) {
    if (task >= 512) return 0;
    if (!p->worker_tasks.objects[task] && p->worker_tasks.root[task]) {
        if (!p->old_driver) {
            p->old_driver = 1;
            fprintf(stderr, "hengevm: pvg-lite: an older guest GPU driver (no object list command): its own layouts\n");
        }
        return OLD_DRIVER_OBJECTS;
    }
    return p->worker_tasks.objects[task];
}

int hv_pvg_lite_old_driver(HvPvgLite *p) {
    return p ? p->old_driver : 0;
}

/* A stamp Metal finished: the guest sees it and the next poll raises the interrupt. */
void hv_pvg_lite_complete(HvPvgLite *p, uint32_t ch, uint32_t stamp) {
    uint64_t buf = (uint64_t)reg(p, 0x1030) << 14;
    uint32_t *s = buf ? guest32_w(p, buf + 4 * ch) : NULL;
    if (s) __atomic_store_n(s, stamp, __ATOMIC_RELEASE), hv_dma_written(p->m, buf + 4 * ch, 4);
    /* The interrupt now, from this thread: the GIC wakes the CPU it goes to. Left for
       CPU 0's next poll this could wait for its timer, up to 10 ms if it is idle, and
       the guest waits for this between the many batches of work in a frame. */
    __atomic_fetch_or(&p->status, 1u << ch, __ATOMIC_ACQ_REL);
    hv_gicv3_pend(p->gic, p->gfx_irq);
    if (p->m->ncpus == 1) hv_machine_wake(p->m);
}

static int offered_mode(uint32_t w, uint32_t h) {
    for (uint32_t i = 0; i < offered_n; i++)
        if (offered[i][0] == w && offered[i][1] == h) return 1;
    return 0;
}

/* The display runs at one of the offered modes. A picture of that size is shown as it is; any other
   size is one of macOS's scaled resolutions (or one it remembered from an earlier panel), drawn at that
   size and stretched over the panel's own mode, as a Mac's display hardware does. The pointer's
   position (page 0xe00) and the cursor's glyph are in the mode's pixels, not the picture's. */
void hv_pvg_lite_pointer_space(uint32_t surface_w, uint32_t surface_h, uint32_t *w, uint32_t *h) {
    int same = offered_n ? offered_mode(surface_w, surface_h) : 1;
    *w = same ? surface_w : display_w;
    *h = same ? surface_h : display_h;
}

static int surface_size(HvPvgLite *p, uint32_t *w, uint32_t *h) {
    HvPvgMetal *metal = p ? atomic_load_explicit(&p->metal, memory_order_acquire) : NULL;
    if (metal && hv_pvg_metal_screen_size(metal, w, h) == 0) return 0;
    if (!p || !p->display_pa) return -1;
    *w = display_w, *h = display_h; /* what macOS starts in, until it shows something */
    return 0;
}

int hv_pvg_lite_picture_size(HvPvgLite *p, uint32_t *w, uint32_t *h) { return surface_size(p, w, h); }

int hv_pvg_lite_screen_size(HvPvgLite *p, uint32_t *w, uint32_t *h) {
    uint32_t sw, sh;
    if (surface_size(p, &sw, &sh) != 0) return -1;
    hv_pvg_lite_pointer_space(sw, sh, w, h);
    return 0;
}

int hv_pvg_lite_retina(void) { return offered_dense; }

/* macOS draws the offered modes at 2x on a Retina-dense panel; other sizes it shows as they are. */
int hv_pvg_lite_screen_points(HvPvgLite *p, uint32_t *w, uint32_t *h) {
    uint32_t sw, sh;
    if (surface_size(p, &sw, &sh) != 0) return -1;
    uint32_t k = offered_dense && offered_mode(sw, sh) ? 2 : 1;
    *w = sw / k, *h = sh / k;
    return 0;
}

void hv_pvg_lite_hide_pointer(HvPvgLite *p, int hide) {
    if (!p) return;
    /* Asked before macOS first draws (a touch device starts with no pointer): the renderer takes it when made. */
    os_unfair_lock_lock(&p->layer_lock);
    p->pointer_hidden = hide != 0;
    HvPvgMetal *metal = atomic_load_explicit(&p->metal, memory_order_acquire);
    os_unfair_lock_unlock(&p->layer_lock);
    if (metal) hv_pvg_metal_hide_pointer(metal, hide);
}

void hv_pvg_lite_set_layer(HvPvgLite *p, void *layer) {
    if (!p) return;
    CFTypeRef retained = layer ? CFRetain(layer) : NULL;
    os_unfair_lock_lock(&p->layer_lock);
    CFTypeRef previous = p->layer;
    p->layer = retained;
    p->layer_set = 1;
    HvPvgMetal *metal = atomic_load_explicit(&p->metal, memory_order_acquire);
    if (metal) hv_pvg_metal_set_layer(metal, layer);
    os_unfair_lock_unlock(&p->layer_lock);
    if (previous) CFRelease(previous);
}

void hv_pvg_lite_barrier_stats(HvPvgLite *p, uint64_t out[4]) {
    out[0] = p->barriers, out[1] = p->barrier_drains, out[2] = p->barrier_retries, out[3] = p->barrier_cycles;
}

uint64_t hv_pvg_lite_gpu_frames(HvPvgLite *p) {
    return p && p->metal ? hv_pvg_metal_gpu_frames(p->metal) : 0;
}

int hv_pvg_lite_gpu_faulted(HvPvgLite *p) {
    return p && p->metal ? hv_pvg_metal_gpu_faulted(p->metal) : 0;
}

void hv_pvg_lite_gpu_rearm(HvPvgLite *p) {
    if (p && p->metal) hv_pvg_metal_gpu_rearm(p->metal);
}

int hv_pvg_lite_gpu_state(HvPvgLite *p) {
    return p && p->metal ? hv_pvg_metal_gpu_state(p->metal) : HV_PVG_GPU_RUNS;
}

int hv_pvg_lite_gpu_try_again(HvPvgLite *p) {
    return p && p->metal ? hv_pvg_metal_gpu_try_again(p->metal) : -1;
}

int hv_pvg_lite_hold(HvPvgLite *p, uint64_t wait_ns) {
    return p && p->metal ? hv_pvg_metal_hold(p->metal, wait_ns) : 0;
}

int hv_pvg_lite_release(HvPvgLite *p) {
    return p && p->metal ? hv_pvg_metal_release(p->metal) : 0;
}

int hv_pvg_lite_gpu_code_refused(HvPvgLite *p) {
    return p && p->metal ? hv_pvg_metal_gpu_code_refused(p->metal) : 0;
}

int hv_pvg_lite_pointer_shown(HvPvgLite *p) {
    return p && p->metal ? hv_pvg_metal_pointer_shown(p->metal) : 0;
}

int hv_pvg_lite_in_use(HvPvgLite *p) {
    return p && (reg(p, 0x1000) != 0 || p->io_regs[0x30 / 8] != 0);
}

int hv_pvg_lite_snapshot(HvPvgLite *p, uint8_t *bgra, uint32_t width, uint32_t height) {
    return p && p->metal ? hv_pvg_metal_snapshot(p->metal, bgra, width, height) : -1;
}

void hv_pvg_lite_lab(HvPvgLite *p, int readback, const char *when) {
    HvPvgMetal *metal = p ? atomic_load_explicit(&p->metal, memory_order_acquire) : NULL;
    if (metal) hv_pvg_metal_lab(metal, readback, when);
}

void hv_pvg_lite_stages(HvPvgLite *p, uint32_t ms[HV_PVG_STAGES]) {
    memset(ms, 0, HV_PVG_STAGES * sizeof *ms);
    HvPvgMetal *metal = p ? atomic_load_explicit(&p->metal, memory_order_acquire) : NULL;
    if (metal) hv_pvg_metal_stages(metal, ms);
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), oldest = 0;
    for (int i = 0; p && i < MAX_CHANNELS; i++) {
        uint64_t t = atomic_load_explicit(&p->wait_since[i], memory_order_relaxed);
        if (t && (!oldest || t < oldest)) oldest = t;
    }
    if (oldest && now > oldest) ms[HV_PVG_STAGE_BARRIER] = (uint32_t)((now - oldest) / 1000000 > UINT32_MAX ? UINT32_MAX : (now - oldest) / 1000000);
}

int hv_pvg_lite_frame(HvPvgLite *p, uint8_t *to, size_t cap, uint32_t *width, uint32_t *height) {
    HvPvgMetal *metal = p ? atomic_load_explicit(&p->metal, memory_order_acquire) : NULL;
    return metal ? hv_pvg_metal_frame(metal, to, cap, width, height) : -1;
}

static void tally_object(HvPvgLite *p, uint32_t task, uint32_t ref, const char *why) {
    if (task >= 512 || ref >= 8192 || !ref || p->obj_seen[task][ref]) return;
    p->obj_seen[task][ref] = 1;
    uint32_t e[3] = {0}, pl[6] = {0};
    if (task_read(p, &p->tasks, task, p->tasks.objects[task] + ref * 12ull, e, 12)) return;
    task_read(p, &p->tasks, task, p->tasks.placement[task] + ref * 24ull, pl, 24);
    uint64_t off = (uint64_t)e[2] << 32 | e[1];
    uint32_t d[36] = {0};
    int have = task_read(p, &p->tasks, task, off, d, sizeof d) == 0;
    fprintf(stderr, "pvgobj %s task %u ref %u entry %08x %08x %08x placement %08x %08x %08x %08x %08x %08x data(%d)", why, task, ref, e[0], e[1], e[2], pl[0], pl[1], pl[2], pl[3], pl[4], pl[5], have);
    if (have) for (int i = 0; i < 36; i++) fprintf(stderr, " %08x", d[i]);
    fprintf(stderr, "\n");
}

/* HV_PVG_TALLY: which serialized commands the guest uses. Encoder types: 0
   render, 1 compute, 2 blit, 4 info. */
static void tally_buffer(HvPvgLite *p, uint32_t task, uint64_t off, uint64_t len) {
    uint32_t hdr[2];
    uint64_t at = off, end = off + len;
    if (task_read(p, &p->tasks, task, at, hdr, 8)) {
        if (p->bad++ < 6) {
            const uint32_t *root = task < 512 && p->tasks.root[task] ? hv_phys_ptr_r(p->m, (uint64_t)p->tasks.root[task] << 14, 0x4000) : NULL;
            const uint32_t *t0 = root ? hv_phys_ptr_r(p->m, (uint64_t)root[0] << 14, 0x4000) : NULL;
            if (t0) {
                fprintf(stderr, "pvgtally top:");
                for (int i = 0; i < 8; i++) fprintf(stderr, " %x", t0[i]);
                const uint32_t *t1 = t0[0] ? hv_phys_ptr_r(p->m, (uint64_t)(t0[0] & 0x7fffffff) << 14, 0x4000) : NULL;
                if (t1) {
                    fprintf(stderr, " leaf[%llx]:", (unsigned long long)(off >> 14));
                    for (int i = -2; i < 4; i++) fprintf(stderr, " %x", t1[(off >> 14) + i]);
                }
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "pvgtally read failed: task %u off %llx len %llx root page %x root %x %x %x %x %x %x\n", task, (unsigned long long)off, (unsigned long long)len,
                    task < 512 ? p->tasks.root[task] : 0, root ? root[0] : 0, root ? root[1] : 0, root ? root[2] : 0, root ? root[3] : 0, root ? root[4] : 0, root ? root[5] : 0);
        }
        return;
    }
    at += 8;
    uint32_t type = hdr[1] & 0xff, cont = (hdr[1] >> 8) & 0xff;
    if (type == 5) { /* protection options, then the real header */
        at += 8;
        if (task_read(p, &p->tasks, task, at, hdr, 8)) { p->bad++; return; }
        at += 8;
        type = hdr[1] & 0xff, cont = (hdr[1] >> 8) & 0xff;
    }
    if (type > 5) { p->bad++; return; }
    uint64_t seg_end = at + hdr[0];
    if (seg_end > end) seg_end = end;
    if (type == 0 && !cont) { /* a render pass description comes first: {tag, size} records until tag 0xffffffff? */
        uint32_t r[2];
        if (task_read(p, &p->tasks, task, at, r, 8)) { p->bad++; return; }
        p->counts[5][r[0] & 0xff]++; /* its first word, to learn its shape */
        static int shown;
        if (shown++ < 3) {
            uint32_t w[72] = {0};
            task_read(p, &p->tasks, task, at, w, sizeof w);
            fprintf(stderr, "pvgpass");
            for (int i = 0; i < 72; i++) fprintf(stderr, " %08x", w[i]);
            fprintf(stderr, "\n");
        }
    }
    while (at + 8 <= seg_end) {
        uint32_t r[2];
        if (task_read(p, &p->tasks, task, at, r, 8)) { p->bad++; return; }
        if (r[1] < 8 || at + r[1] > seg_end) { p->bad++; break; }
        if (r[0] < 256) p->counts[type][r[0]]++;
        if (type == 0) { /* the objects it names */
            uint32_t a[32] = {0}, c = r[1] - 8 > sizeof a ? sizeof a : r[1] - 8;
            task_read(p, &p->tasks, task, at + 8, a, c);
            if (r[0] == 116) tally_object(p, task, a[0], "pipeline");
            if (r[0] == 114 || r[0] == 161) for (uint32_t i = 0; i < a[1] && i < 16; i++) tally_object(p, task, a[2 + i], "texture");
            if (r[0] == 112) for (uint32_t i = 0; i < a[1] && i < 16; i++) tally_object(p, task, a[2 + i], "sampler");
            if (r[0] == 125 || r[0] == 110 || r[0] == 157)
                for (uint32_t i = 0; i < a[1] && i < 6; i++) tally_object(p, task, *(uint32_t *)((uint8_t *)a + 8 + i * 12), "buffer");
            if (r[0] == 7) tally_object(p, task, a[1], "indexbuf");
        }
        at += r[1];
    }
}

static void tally_exec(HvPvgLite *p, const uint32_t *table, uint32_t size, uint32_t rd, uint32_t len, uint32_t skip) {
    uint32_t a[3];
    ring_read(p, table, size, rd + 12 + skip, a, 12);
    uint32_t task = a[0], resources = a[1], buffers = a[2];
    if (buffers > 64 || 12 + skip + 12 + resources * 8 + buffers * 16 > len) { p->bad++; return; }
    for (uint32_t i = 0; i < buffers; i++) {
        uint64_t b[2];
        ring_read(p, table, size, rd + 12 + skip + 12 + resources * 8 + i * 16, b, 16);
        tally_buffer(p, task, b[0], b[1]);
    }
    p->execs++;
    if (p->execs % 2000 == 0) {
        fprintf(stderr, "pvgtally execs %llu bad %llu\n", (unsigned long long)p->execs, (unsigned long long)p->bad);
        for (int t = 0; t < 6; t++)
            for (int i = 0; i < 256; i++)
                if (p->counts[t][i]) fprintf(stderr, "pvgtally %d %d %llu\n", t, i, (unsigned long long)p->counts[t][i]);
    }
}

/* Commands that do something even without a GPU to run them. */
static void channel_command(HvPvgLite *p, const uint32_t *table, uint32_t size, uint32_t rd, const uint32_t *hdr) {
    uint32_t op = hdr[0] & 0xffff, args[32], skip = (hdr[0] >> 16) * 8; /* the barriers come first */
    uint32_t n = hdr[1] > 12 + skip ? hdr[1] - 12 - skip : 0;
    if (n > sizeof args) n = sizeof args;
    if (op == 0x37) {
        if (!p->old_driver) {
            p->old_driver = 1;
            fprintf(stderr, "hengevm: pvg-lite: older guest GPU driver (op 0x37): compact layouts enabled\n");
        }
    }
    if ((op == 0x38 && n < 16) || (op == 0x44 && n < 28)) return; /* shorter than what it says */
    if (op == 0x38) {
        ring_read(p, table, size, rd + 12 + skip, args, 16);
        task_defined(p, args);
        return;
    }
    if (op == 0x44) {
        ring_read(p, table, size, rd + 12 + skip, args, 28);
        tasks_apply(&p->tasks, 0x44, args, 28);
        if (args[0] < 512 && p->tally) fprintf(stderr, "pvgobj list task %u objects %llx len %x placement %llx len %x\n", args[0], (unsigned long long)p->tasks.objects[args[0]], args[5], (unsigned long long)p->tasks.placement[args[0]], args[6]);
        return;
    }
    if (op == 0x43 && p->tally) tally_exec(p, table, size, rd, hdr[1], skip);
    if (p->metal) return; /* in order, on its worker */
    if (op != 1 && op != 2 && op != 8) return; /* the cursor needs the Metal side */
    memset(args, 0, sizeof args);
    ring_read(p, table, size, rd + 12 + skip, args, n);
    display_command(p, op, args, n);
}

/* A mapping id is 1 to 0x1fff, as Apple's host takes it: -[PGIOSurfaceHostDevice mapSurface:] and unmapSurface:
   answer any other with 0xe00002c2 (kIOReturnBadArgument) and keep nothing for it, and its table has that many slots.
   So what the Metal side keeps by id (its surfaces, and the ids unmapped and not mapped again) is at most that many,
   whatever ids the guest's ring names. macOS's own ids stay within it: they reach 5487 in the 9 Nov lag log
   (a device log), with none refused. */
#define PVG_MAPPING_IDS HV_PVG_MAPPING_IDS
static int mapping_id(uint64_t id) { return id - 1 < PVG_MAPPING_IDS - 1; }

/* The mapper's table: a u32 per 16 KB of descriptors, physical address >> 12 with bit 0 meaning present. */
const uint8_t *hv_pvg_lite_surface(HvPvgLite *p, uint64_t mapping) {
    if (!mapping_id(mapping)) return NULL; /* none of the ids the mapper takes: no surface, as with Apple's host */
    /* macOS 14's (and older) driver writes 0x200-byte descriptions, 32 to a page; later ones 0x400, 16 to a page. Reading the
       older ones as 0x400 apart put id 2k on the first half of "slot" k and never read id 2k+1 at all. */
    uint32_t per = p->old_driver ? 32 : 16, size = 0x4000 / per;
    uint64_t table = p->io_regs[0x10 / 8];
    const uint32_t *e = table ? hv_phys_ptr_r(p->m, table + (mapping / per) * 4ull, 4) : NULL;
    if (!e || !(*e & 1)) return NULL;
    return hv_phys_ptr_r(p->m, ((uint64_t)(*e & ~1u) << 12) + (mapping % per) * (uint64_t)size, size);
}

const uint8_t *hv_pvg_lite_display_page(HvPvgLite *p) {
    return p->display_pa ? hv_phys_ptr_r(p->m, p->display_pa, 0x4000) : NULL; /* read by the presenter, every frame */
}

int hv_pvg_lite_command(HvPvgLite *p, const uint32_t *cmd, uint32_t len) {
    if (len < 12) return len < 4 || !display_least(cmd[0] & 0xffff); /* no arguments: no display command taken */
    uint32_t op = cmd[0] & 0xffff, skip = (cmd[0] >> 16) * 8;
    uint32_t n = len > 12 + skip ? len - 12 - skip : 0;
    uint32_t args[10] = {0}; /* the most display_command and display_swap read; a shorter command is refused */
    memcpy(args, (const uint8_t *)cmd + 12 + skip, n < sizeof args ? n : sizeof args);
    return display_least(op) ? display_command(p, op, args, n) : 1;
}

/* The Metal side, made the first time the guest gives the GPU work (or a snapshot is restored). */
static void ensure_metal(HvPvgLite *p) {
    os_unfair_lock_lock(&p->layer_lock);
    if (!p->metal_tried) {
        p->metal_tried = 1;
        p->worker_tasks = p->tasks; /* a worker with nothing queued: every command read so far is its own */
        /* No Metal side with either switch (gpu_switch.h): HV_NO_GPU keeps the GPU off on a host (the MacBook) */
        HvPvgMetal *metal = hv_gpu_switched_off() ? NULL : hv_pvg_metal_create(p);
        /* The mapper's state as its own, first: the ops walked before it existed (or before a snapshot was taken) are
           in it, and any after come in order behind this. Both run under the device lock (do_mapper, do_channel, this),
           or before the CPUs do (hv_pvg_lite_restored), so nothing changes it meanwhile. */
        if (metal) hv_pvg_metal_set_unmapped(metal, p->unmapped);
        if (metal && p->layer_set) hv_pvg_metal_set_layer(metal, (void *)p->layer);
        if (metal && p->pointer_hidden) hv_pvg_metal_hide_pointer(metal, 1);
        atomic_store_explicit(&p->metal, metal, memory_order_release);
    }
    os_unfair_lock_unlock(&p->layer_lock);
}

/* A channel with no ring (the guest took it down) or one walked to its end: nothing on it waits any more */
static void chan_not_waiting(HvPvgLite *p, uint32_t ch) {
    p->waits_on[ch] = 0;
    atomic_store_explicit(&p->wait_since[ch], 0, memory_order_relaxed);
}

static void do_channel(HvPvgLite *p, uint32_t ch) {
    if (!ch || ch >= MAX_CHANNELS) return;
    uint64_t root = (uint64_t)reg(p, 0x101c) << 14, buf = (uint64_t)reg(p, 0x1030) << 14;
    if (!root || !buf) { chan_not_waiting(p, ch); return; }
    uint32_t *rec = hv_phys_ptr_w(p->m, root + 0x400 + 0x14 * (ch - 1), 0x14); /* its read index is stored */
    if (!rec) { chan_not_waiting(p, ch); return; }
    const uint32_t *table = hv_phys_ptr_r(p->m, (uint64_t)rec[4] << 14, 0x4000);
    if (!table) { chan_not_waiting(p, ch); return; }
    uint32_t pages = 0;
    while (pages < 0x1000 && table[pages]) pages++;
    if (!pages) { chan_not_waiting(p, ch); return; }
    if (p->draining & (1u << ch)) return; /* already being drained, further up */
    p->draining |= 1u << ch;
    uint32_t size = pages << 14, wr = __atomic_load_n(&rec[0], __ATOMIC_ACQUIRE), rd = rec[1], last = 0;
    int any = 0;
    while (rd != wr) {
        uint32_t hdr[3];
        for (int i = 0; i < 3; i++) {
            uint32_t at = (rd + 4u * i) % size;
            const uint32_t *w = guest32(p, ((uint64_t)table[at >> 14] << 14) + (at & 0x3fff));
            hdr[i] = w ? *w : 0;
        }
        /* No shorter than its header and barriers, no longer than what was written: the rest is the guest's next work */
        if (wr - rd > size || !command_length_ok(hdr, wr - rd)) {
            uint32_t n = bad_count(p, PVG_BAD_CHAN_CMD);
            if (n) fprintf(stderr, "hengevm: pvg-lite: channel %u: bad command %x length %x at %x (%u so far, all channels)\n", ch, hdr[0], hdr[1], rd, n);
            rd = wr;
            break;
        }
        if (!command_barriers_ok(hdr)) { /* not run; its stamp reached in order: on the worker, behind the work before it */
            if (!(p->barriers_told & 1u << ch))
                fprintf(stderr, "hengevm: pvg-lite: channel %u: command %x of length %x: its barriers don't fit, refused\n", ch, hdr[0], hdr[1]);
            p->barriers_told |= 1u << ch;
            ensure_metal(p);
            uint32_t *none = NULL;
            if (p->metal && !(none = malloc(12))) { /* left in the ring for the next poll */
                p->chan_pending |= 1u << ch;
                break;
            }
            p->submitted[ch] = hdr[2];
            if (none) {
                none[0] = 0, none[1] = 12, none[2] = hdr[2]; /* op 0, no barriers: nothing done but the stamp */
                hv_pvg_metal_submit(p->metal, p, ch, none, 12);
            } else {
                last = hdr[2], any = 1;
            }
            rd += hdr[1];
            continue;
        }
        if (p->log) fprintf(stderr, "hengevm: pvg-lite: ch %u op %x len %x stamp %x\n", ch, hdr[0], hdr[1], hdr[2]);
        {   /* HV_PVG_TRACE_FILE exists: the order commands reach the device in, with what a swap says */
            if (hv_pvg_tracing()) {
                uint32_t w[12] = {0};
                ring_read(p, table, size, rd + 12, w, hdr[1] - 12 < sizeof w ? hdr[1] - 12 : sizeof w);
                fprintf(stderr, "trace: dev ch %u op %x len %u stamp %x sub %x args %x %x %x %x %x %x %x %x\n", ch, hdr[0], hdr[1], hdr[2], p->submitted[ch], w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
        ensure_metal(p);
        int blocked = 0;
        for (uint32_t k = 0; p->metal && k < hdr[0] >> 16 && !blocked; k++) {
            /* {channel, stamp}, as many as bits 31..16 say: this waits for that channel's
               work, such as a swap for the drawing of the frame it shows. All of it goes
               through one worker in order, so everything up to the stamp has to be handed
               over first, or the screen shows a frame half drawn. */
            uint32_t b[2] = {0, 0};
            ring_read(p, table, size, rd + 12 + 8 * k, b, 8);
            if (!barrier_met(p, ch, b[0], b[1])) { /* not written yet: this channel waits for a later poll */
                p->chan_pending |= 1u << ch;
                blocked = 1;
            }
        }
        if (blocked) break;
        p->waits_on[ch] = 0;
        atomic_store_explicit(&p->wait_since[ch], 0, memory_order_relaxed);
        uint32_t *copy = NULL;
        if (p->metal && !(copy = malloc(hdr[1]))) { /* the guest's size: left in the ring for the next poll */
            p->chan_pending |= 1u << ch;
            break;
        }
        channel_command(p, table, size, rd, hdr);
        p->submitted[ch] = hdr[2];
        if (copy) {
            ring_read(p, table, size, rd, copy, hdr[1]);
            hv_pvg_metal_submit(p->metal, p, ch, copy, hdr[1]);
        } else {
            last = hdr[2], any = 1;
        }
        rd += hdr[1];
    }
    if (rd == wr) chan_not_waiting(p, ch); /* all walked (a bad command skips the rest) */
    __atomic_store_n(&rec[1], rd, __ATOMIC_RELEASE);
    /* Written, then marked (as every device): rec was lent for the whole drain, and another device's mark on the
       page may have ended that lend meanwhile */
    hv_dma_written(p->m, root + 0x400 + 0x14 * (ch - 1), 0x14);
    p->draining &= ~(1u << ch);
    if (any) {
        uint32_t *stamp = guest32_w(p, buf + 4 * ch);
        if (stamp) __atomic_store_n(stamp, last, __ATOMIC_RELEASE), hv_dma_written(p->m, buf + 4 * ch, 4);
        __atomic_fetch_or(&p->status, 1u << ch, __ATOMIC_ACQ_REL);
    }
}

/* The mapper's ring: surfaces coming and going. The guest waits for its interrupt.
   Entries are 16 bytes {op (1 map, 2 unmap, 3 test: never supported), id, result, 0}; the guest's indexes count up without
   end and the ring holds size of them. Each is walked once, from mapper_rp to what was submitted. An unmap
   may still be in use by GPU work handed over before it, so with Metal it completes only once that work is
   done (a fence on the worker; the vCPU doesn't wait), and whatever comes after it then waits its turn,
   for the guest has one completed index.
   At most HV_PVG_MAPPER_BATCH entries a call: each queues a block for the worker, and this runs in the guest's MMIO
   write with the device lock held, so a guest with a ring of millions would otherwise hold the lock (and every
   vCPU's device access) that long, and queue that many blocks at once. The rest is walked by the next polls
   (hv_pvg_lite_poll), a batch each, and the guest's completed index moves only once all is walked, as before. */
#ifndef HV_PVG_MAPPER_BATCH
#define HV_PVG_MAPPER_BATCH 4096
#endif
/* The mapper's walk moved past entries it did not read (the guest's ring restarted, a submitted index that isn't a ring
   position, a ring that isn't in guest memory): whatever they mapped or unmapped is not known, so no id is held to be
   unmapped any more, here and on the Metal side. That is how it was before the state was kept: a texture over any id is
   made from its descriptor. */
static void mapper_unknown(HvPvgLite *p, HvPvgMetal *metal) {
    memset(p->unmapped, 0, sizeof p->unmapped);
    if (metal) hv_pvg_metal_mapper_reset(metal);
}

static void do_mapper(HvPvgLite *p) {
    uint64_t ring = p->io_regs[0], table = p->io_regs[0x10 / 8], size = p->io_regs[0x8 / 8];
    uint64_t want = p->io_regs[0x18 / 8];
    HvPvgMetal *metal = hv_pvg_lite_metal(p);
    if (want - p->mapper_rp > size) { /* behind, or further ahead than the ring holds: not ring positions */
        uint32_t n = bad_count(p, PVG_BAD_MAPPER_WANT);
        if (n)
            fprintf(stderr, "hengevm: pvg-lite: mapper submitted %llx is not within a ring of %llx (%u so far)\n", (unsigned long long)want,
                    (unsigned long long)p->mapper_rp, n);
        /* as a restart of the ring (0x1020) is: a fence asked for before completes nothing of what comes after */
        p->mapper_rp = p->mapper_fenced = want;
        atomic_fetch_add(&p->mapper_gen, 1);
        mapper_unknown(p, metal);
    }
    const uint32_t *entries = size && size <= UINT64_MAX / 16 ? hv_phys_ptr_r(p->m, ring, size * 16) : NULL;
    uint32_t n = !entries && p->mapper_rp != want ? bad_count(p, PVG_BAD_MAPPER_RING) : 0; /* each poll tries it again */
    if (n)
        fprintf(stderr, "hengevm: pvg-lite: mapper ring %llx (%llu entries) is not in guest memory (%u so far)\n", (unsigned long long)ring,
                (unsigned long long)size, n);
    if (!entries && p->mapper_rp != want) mapper_unknown(p, metal); /* passed over, unread, below */
    int unmaps = p->mapper_unmapped;
    uint64_t walked = 0;
    for (; entries && p->mapper_rp != want && walked < HV_PVG_MAPPER_BATCH; p->mapper_rp++, walked++) {
        const uint32_t *e = entries + (p->mapper_rp % size) * 4;
        uint32_t op = e[0], id = e[1];
        if (p->tally) {
            const uint32_t *t = table ? hv_phys_ptr_r(p->m, table, 64) : NULL;
            if (t) fprintf(stderr, "pvgmap op %u id %u table %x %x %x %x\n", op, id, t[0], t[1], t[2], t[3]);
            const uint32_t *ta = table ? hv_phys_ptr_r(p->m, table + (id / 16) * 4ull, 4) : NULL;
            const uint32_t *d = ta ? hv_phys_ptr_r(p->m, ((uint64_t)(*ta & ~1u) << 12) + (id % 16) * 0x400ull, 0x400) : NULL;
            if (d && op == 1) {
                fprintf(stderr, "pvgdesc");
                for (int k = 0; k < 64; k++) fprintf(stderr, " %08x", d[k]);
                fprintf(stderr, "\n");
            }
        }
        /* Apple's wakeDevice answers any op but 1 and 2 with kIOReturnUnsupported, and so does its testSurface: (3) */
        uint32_t answer = op == 1 || op == 2 ? 0 : 0xe00002c7;
        if ((op == 1 || op == 2) && !mapping_id(id)) {
            answer = 0xe00002c2;
            if (!p->mapping_told) fprintf(stderr, "hengevm: pvg-lite: mapper op %u for id %u, outside 1-%u: refused\n", op, id, PVG_MAPPING_IDS - 1);
            p->mapping_told = 1;
        } else if (op == 1) {
            p->unmapped[id / 8] &= (uint8_t)~(1u << (id % 8));
            if (metal) hv_pvg_metal_map(metal, id);
        } else if (op == 2) {
            p->unmapped[id / 8] |= (uint8_t)(1u << (id % 8));
            if (metal) hv_pvg_metal_unmap(metal, id), unmaps = 1;
        }
        /* its result: done, or refused (claimed first: an auto-save's guard keeps the page as it was) */
        uint32_t *result = hv_phys_ptr_w(p->m, ring + (p->mapper_rp % size) * 16 + 8, 4);
        if (result) __atomic_store_n(result, answer, __ATOMIC_RELAXED), hv_dma_written(p->m, ring + (p->mapper_rp % size) * 16 + 8, 4);
    }
    if (entries && p->mapper_rp != want) { /* the rest at the next poll */
        p->mapper_more = 1, p->mapper_unmapped = unmaps;
        return;
    }
    p->mapper_more = 0, p->mapper_unmapped = 0;
    p->mapper_rp = want;
    uint64_t done = __atomic_load_n(&p->io_regs[0x20 / 8], __ATOMIC_ACQUIRE);
    if (metal && (unmaps || p->mapper_fenced != done)) {
        p->mapper_fenced = want;
        hv_pvg_metal_mapper_fence(metal, want, atomic_load(&p->mapper_gen));
        return;
    }
    p->mapper_fenced = want;
    __atomic_store_n(&p->io_regs[0x20 / 8], want, __ATOMIC_RELEASE);
    hv_gicv3_pend(p->gic, p->iosfc_irq);
}

/* The GPU finished the work before a mapper fence: the guest's completed index moves to it, if the guest
   hasn't started its ring again since. From the Metal side's thread, never one the device lock holder waits for. */
void hv_pvg_lite_mapper_done(HvPvgLite *p, uint64_t index, uint64_t gen) {
    hv_dev_lock(p->m);
    uint64_t done = __atomic_load_n(&p->io_regs[0x20 / 8], __ATOMIC_ACQUIRE);
    int moved = gen == atomic_load(&p->mapper_gen) && index != done && index - done <= p->mapper_fenced - done;
    if (moved) __atomic_store_n(&p->io_regs[0x20 / 8], index, __ATOMIC_RELEASE);
    hv_dev_unlock(p->m);
    if (moved) {
        hv_gicv3_pend(p->gic, p->iosfc_irq);
        if (p->m->ncpus == 1) hv_machine_wake(p->m);
    }
}

/* Device work no doorbell took care of: a channel that had to wait for another one's work. */
/* Also work the guest added with no doorbell: its driver rings only for a ring it found empty, so work added
   while an earlier doorbell was being answered would otherwise wait for the next one. */
/* Experiment (HV_PVG_VBLANK=1): the display's refresh event. The guest asks for event 8 in its mask (+104) while
   something waits on the display's timing (a display link); without it a video wallpaper stops when nothing else
   draws. Sent at the mode's rate while asked for. */
static void vblank(HvPvgLite *p) {
    if (!p || !p->display_pa) return;
    static int env_checked = 0;
    static uint64_t base_period, period, next;
    static int last_thermal = -1;
    if (!env_checked) {
        env_checked = 1;
        p->vblank_on = 1;
        const char *e = getenv("HV_PVG_VBLANK");
        if (e && !atoi(e)) p->vblank_on = 0;
        const char *hz = getenv("HV_HZ");
        uint64_t r = hz && atoi(hz) > 0 ? (uint64_t)atoi(hz) : 60;
        base_period = 24000000ull / r; /* host counter ticks (24 MHz) */
        period = base_period;
    }
    if (!p->vblank_on) return;
    int heat = hv_thermal_get_state();
    if (heat != last_thermal) {
        last_thermal = heat;
        if (heat >= 3) {
            /* Critical: clamp refresh to 30 Hz (800,000 ticks) */
            uint64_t crit_period = 24000000ull / 30;
            period = crit_period > base_period ? crit_period : base_period;
        } else if (heat >= 2) {
            /* Hot / Serious: clamp refresh to 60 Hz (400,000 ticks) */
            uint64_t hot_period = 24000000ull / 60;
            period = hot_period > base_period ? hot_period : base_period;
        } else {
            period = base_period;
        }
    }
    uint8_t *d = hv_phys_ptr_w(p->m, p->display_pa, 0x4000);
    if (!d || !(__atomic_load_n((uint32_t *)(d + 0x104), __ATOMIC_ACQUIRE) & 8)) return;
    uint64_t now = mach_absolute_time();
    if (now < next) return;
    next = now + period;
    __atomic_fetch_or((uint32_t *)(d + 0x100), 8, __ATOMIC_RELEASE);
    p->vblanks++;
    hv_gicv3_pend(p->gic, p->gfx_irq);
    if (p->m && p->m->ncpus == 1) hv_machine_wake(p->m);
}

void hv_pvg_lite_set_vblank(HvPvgLite *p, int on) {
    if (p) p->vblank_on = on;
}

static HV_HOST_FP_WORK void poll_work(HvPvgLite *p);
void hv_pvg_lite_poll(HvPvgLite *p) { /* CPU 0's tick, under the guest's FPCR: as the doorbells (gfx_write) */
    if (!p) return;
    HvHostFp fp = hv_host_fp_begin();
    poll_work(p);
    hv_host_fp_end(fp);
}

static HV_HOST_FP_WORK void poll_work(HvPvgLite *p) {
    vblank(p);
    if (p->mapper_more) do_mapper(p); /* a long ring's next batch */
    if (p->root_waiting) do_root(p); /* a root command whose barrier was not met */
    uint32_t chans = p->chan_pending;
    uint64_t root = (uint64_t)reg(p, 0x101c) << 14;
    const uint32_t *recs = root ? hv_phys_ptr_r(p->m, root + 0x400, 0x14 * (MAX_CHANNELS - 1)) : NULL;
    for (uint32_t ch = 1; recs && ch < MAX_CHANNELS; ch++) {
        const uint32_t *rec = recs + 5 * (ch - 1);
        if (rec[4] && __atomic_load_n(&rec[0], __ATOMIC_ACQUIRE) != __atomic_load_n(&rec[1], __ATOMIC_ACQUIRE))
            chans |= 1u << ch;
    }
    if (chans) {
        p->chan_pending = 0;
        for (uint32_t ch = 1; ch < MAX_CHANNELS; ch++)
            if (chans & (1u << ch)) do_channel(p, ch);
        if (__atomic_load_n(&p->status, __ATOMIC_ACQUIRE)) hv_gicv3_pend(p->gic, p->gfx_irq);
    }
}

static uint64_t gfx_read(void *opaque, uint64_t off, unsigned size) {
    HvPvgLite *p = opaque;
    uint64_t v = 0;
    switch (off) {
    case 0x100c:
        do_root(p); /* the driver spins on this waiting for ring space */
        v = p->root_rp;
        break;
    case 0x1014:
        v = __atomic_exchange_n(&p->poke, 0, __ATOMIC_ACQ_REL);
        break;
    case 0x1018:
        v = __atomic_exchange_n(&p->status, 0, __ATOMIC_ACQ_REL);
        break;
    case 0x122c: v = 1; break;
    default:
        if (off >= 0x1000 && off < 0x2000) v = p->regs[(off - 0x1000) / 4];
        break;
    }
    (void)size;
    return v;
}

/* The doorbells run on the CPU that rang, under the guest's FPCR: display_set_page computes the display's
   modes in floating point, and Metal's hooks are Apple's code. So the host's, around all of it (host_fp.h). */
static HV_HOST_FP_WORK void gfx_write_work(HvPvgLite *p, uint64_t off, uint64_t v);
static void gfx_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)size;
    HvHostFp fp = hv_host_fp_begin();
    gfx_write_work(opaque, off, v);
    hv_host_fp_end(fp);
}

static HV_HOST_FP_WORK void gfx_write_work(HvPvgLite *p, uint64_t off, uint64_t v) {
    if (off >= 0x1000 && off < 0x2000) p->regs[(off - 0x1000) / 4] = (uint32_t)v;
    /* Doorbells are answered here, on the CPU that rang: no waiting for CPU 0 to come by. */
    if (off == 0x1008) {
        do_root(p);
        hv_gicv3_pend(p->gic, p->gfx_irq);
    } else if (off == 0x1020 && v < MAX_CHANNELS) {
        do_channel(p, (uint32_t)v);
        if (__atomic_load_n(&p->status, __ATOMIC_ACQUIRE)) hv_gicv3_pend(p->gic, p->gfx_irq);
    } else if (p->log && off != 0x1020) {
        fprintf(stderr, "hengevm: pvg-lite: write +%llx = %llx\n", (unsigned long long)off, (unsigned long long)v);
    }
    if (off == 0x1220 && p->metal) hv_pvg_metal_cursor_moved(p->metal); /* the display's doorbell: the pointer moved */
}

static uint64_t iosfc_read(void *opaque, uint64_t off, unsigned size) {
    HvPvgLite *p = opaque;
    (void)size;
    if (off == 0x1028) return 1;
    return off >= 0x1000 && off < 0x1040 ? __atomic_load_n(&p->io_regs[(off - 0x1000) / 8], __ATOMIC_ACQUIRE) : 0;
}

static HV_HOST_FP_WORK void iosfc_write_work(HvPvgLite *p, uint64_t off, uint64_t v);
static void iosfc_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)size;
    HvHostFp fp = hv_host_fp_begin(); /* as gfx_write: Metal's mapper hook */
    iosfc_write_work(opaque, off, v);
    hv_host_fp_end(fp);
}

static HV_HOST_FP_WORK void iosfc_write_work(HvPvgLite *p, uint64_t off, uint64_t v) {
    if (off >= 0x1000 && off < 0x1040) __atomic_store_n(&p->io_regs[(off - 0x1000) / 8], v, __ATOMIC_RELEASE);
    if (off == 0x1020) { /* the guest starting its ring again: a fence asked for before completes nothing */
        p->mapper_rp = p->mapper_fenced = v;
        atomic_fetch_add(&p->mapper_gen, 1);
        mapper_unknown(p, hv_pvg_lite_metal(p)); /* the first at boot too: nothing is held unmapped then anyway */
    }
    if (off == 0x1018) do_mapper(p);
}

HvPvgLite *hv_pvg_lite_create(HvMachine *m, HvGic *gic, uint64_t gfx_pa, uint64_t iosfc_pa, int gfx_irq, int iosfc_irq) {
    HvPvgLite *p = calloc(1, sizeof *p);
    p->layer_lock = OS_UNFAIR_LOCK_INIT;
    atomic_init(&p->metal, NULL);
    p->m = m;
    p->gic = gic;
    p->gfx_irq = gfx_irq;
    p->iosfc_irq = iosfc_irq;
    p->log = getenv("HV_PVG_LITE_LOG") != NULL;
    p->tally = getenv("HV_PVG_TALLY") != NULL;
    hv_machine_add_mmio(m, gfx_pa, 0x10000, p, gfx_read, gfx_write, "gfx");
    hv_machine_add_mmio(m, iosfc_pa, 0x10000, p, iosfc_read, iosfc_write, "iosurface");
    return p;
}

/* ---- Suspend ---- */

void hv_pvg_blob_put(HvPvgBlob *b, const void *v, size_t n) {
    if (b->bad || !n) return;
    if (n > b->cap - b->len) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap - b->len < n) {
            if (cap > SIZE_MAX / 2) { b->bad = 1; return; }
            cap *= 2;
        }
        uint8_t *q = realloc(b->p, cap);
        if (!q) { b->bad = 1; return; }
        b->p = q, b->cap = cap;
    }
    memcpy(b->p + b->len, v, n);
    b->len += n;
}

void hv_pvg_blob_get(HvPvgReader *r, void *v, size_t n) {
    if (r->bad || n > r->len - r->at) {
        r->bad = 1;
        memset(v, 0, n);
        return;
    }
    memcpy(v, r->p + r->at, n);
    r->at += n;
}

HvPvgMetal *hv_pvg_lite_metal(HvPvgLite *p) { return p ? atomic_load_explicit(&p->metal, memory_order_acquire) : NULL; }

#define PVG_MAGIC 0x4c475650u /* "PVGL" */
#define PVG_VERSION 2u /* 2: the GPU's part beside it (a streamed section), not inside: 1 is still read */
#define PUT(f) hv_pvg_blob_put(out, &(f), sizeof(f))
#define GET(f) hv_pvg_blob_get(&in, &(f), sizeof(f))

typedef struct {
    const HvPvgSink *to;
    uint64_t n;
} PvgCount;

static int pvg_count_write(void *ctx, const void *v, size_t n) {
    PvgCount *c = ctx;
    if (c->to->write(c->to->ctx, v, n) != 0) return -1;
    c->n += n;
    return 0;
}

static int pvg_count_room(void *ctx, uint64_t bytes, char *why, size_t why_len) {
    PvgCount *c = ctx;
    return c->to->room ? c->to->room(c->to->ctx, bytes, why, why_len) : 0;
}

void hv_pvg_bytes_release(HvPvgBytes *b) {
    if (!b) return;
    if (b->release) b->release(b->ctx);
    memset(b, 0, sizeof *b);
}

int hv_pvg_lite_save(HvPvgLite *p, HvPvgBlob *out, const HvPvgSink *metal, uint64_t *metal_len, HvPvgBlob *mapper,
                     uint64_t deadline_ns, char *why, size_t why_len) {
    /* The GPU first: finishing its work writes stamps into guest memory and sets status bits */
    HvPvgMetal *mt = hv_pvg_lite_metal(p);
    PvgCount count = {metal, 0};
    HvPvgSink counted = {pvg_count_write, &count, pvg_count_room};
    *metal_len = UINT64_MAX;
    if (p->draining) { /* the CPUs are stopped: no doorbell can be half done (checked first: the GPU's save is long) */
        if (why && why_len) snprintf(why, why_len, "the Mac's screen was in the middle of an update (try again)");
        return -1;
    }
    /* A mapper walk left for the polls finished first: a load takes everything submitted as walked (hv_pvg_lite_load),
       and what it maps and unmaps is queued ahead of the GPU's save */
    while (p->mapper_more) do_mapper(p);
    if (mt && hv_pvg_metal_save(mt, &counted, deadline_ns, why, why_len) != 0) return -1;
    if (p->draining) {
        if (why && why_len) snprintf(why, why_len, "the Mac's screen was in the middle of an update (try again)");
        return -1;
    }
    static const uint32_t no_waits[MAX_CHANNELS];
    uint32_t magic = PVG_MAGIC, version = PVG_VERSION, status = __atomic_load_n(&p->status, __ATOMIC_ACQUIRE);
    PUT(magic);
    PUT(version);
    PUT(p->regs);
    PUT(p->io_regs);
    PUT(p->root_rp);
    PUT(status);
    PUT(p->chan_pending);
    PUT(p->display_pa);
    PUT(p->swaps);
    PUT(p->submitted);
    PUT(no_waits); /* where a count of polls waited was (no longer kept): saved as 0, not loaded */
    PUT(p->tasks.root);
    PUT(p->tasks.objects);
    PUT(p->tasks.placement);
    /* What the guest was told about the display: it keeps those modes, whatever this iPad's screen is now */
    uint32_t dw = display_w, dh = display_h, mw = display_mm_w, mh = display_mm_h, on = offered_n;
    int32_t dense = offered_dense;
    PUT(dw);
    PUT(dh);
    PUT(mw);
    PUT(mh);
    PUT(on);
    PUT(offered);
    PUT(dense);
    uint64_t mlen = mt ? count.n : UINT64_MAX; /* UINT64_MAX: no Metal side yet */
    PUT(mlen);
    *metal_len = mlen;
    if (mapper) hv_pvg_blob_put(mapper, p->unmapped, sizeof p->unmapped); /* after the walk: what metal's records agree with */
    if (out->bad || (mapper && mapper->bad)) {
        if (why && why_len) snprintf(why, why_len, "not enough memory to save the screen");
        return -1;
    }
    return 0;
}

/* A saved screen state, read and checked whole (the GPU's part too); nothing else changes. */
typedef struct PvgParsed {
    uint32_t status, dw, dh, mw, mh, on, off[DISPLAY_MODES_MAX][2];
    int32_t dense;
    const uint8_t *metal; /* inside (version 1); NULL: beside it, or no Metal side yet */
    uint64_t mlen;        /* UINT64_MAX: no Metal side yet */
    uint32_t version;
} PvgParsed;

/* The saved fields into t (only those), the rest into q. 0, or -1 (said why). */
/* ext: the GPU's part beside it (version 2), checked here too */
static int pvg_parse(const uint8_t *data, size_t len, const uint8_t *ext, size_t ext_len, struct HvPvgLite *t, PvgParsed *q) {
    HvPvgReader in = {data, len, 0, 0};
    uint32_t magic = 0, version = 0;
    memset(q, 0, sizeof *q);
    GET(magic);
    GET(version);
    q->version = version;
    if (magic != PVG_MAGIC || (version != 1 && version != PVG_VERSION)) {
        fprintf(stderr, "hengevm: snapshot: the screen's state is from another version of Henge\n");
        return -1;
    }
    GET(t->regs);
    GET(t->io_regs);
    GET(t->root_rp);
    GET(q->status);
    GET(t->chan_pending);
    GET(t->display_pa);
    GET(t->swaps);
    GET(t->submitted);
    GET(t->waits_on); /* a count of polls waited, in saves before 9 Nov 2026: not loaded */
    GET(t->tasks.root);
    GET(t->tasks.objects);
    GET(t->tasks.placement);
    GET(q->dw);
    GET(q->dh);
    GET(q->mw);
    GET(q->mh);
    GET(q->on);
    GET(q->off);
    GET(q->dense);
    GET(q->mlen);
    const uint8_t *gpu = NULL; /* the GPU's part, wherever it is, to check */
    if (!in.bad && q->mlen != UINT64_MAX) {
        if (version == 1) {
            if (q->mlen > in.len - in.at) in.bad = 1;
            else q->metal = gpu = in.p + in.at, in.at += q->mlen;
        } else if (!ext || ext_len != q->mlen) {
            in.bad = 1; /* said to be beside it, and isn't, or not whole */
        } else {
            gpu = ext;
        }
    }
    if (in.bad || in.at != in.len || q->on > DISPLAY_MODES_MAX || !q->dw || !q->dh || q->dw > 16384 || q->dh > 16384 ||
        (t->chan_pending & 1) || (q->dense != 0 && q->dense != 1) || (gpu && hv_pvg_metal_check(gpu, q->mlen) != 0)) {
        fprintf(stderr, "hengevm: snapshot: the screen's state is damaged, or from another version of Henge\n");
        return -1;
    }
    return 0;
}

int hv_pvg_lite_check(const uint8_t *data, size_t len, const uint8_t *metal, size_t metal_len) {
    static struct HvPvgLite t; /* too big for the stack; only before the load, on one thread */
    PvgParsed q;
    return pvg_parse(data, len, metal, metal_len, &t, &q);
}

int hv_pvg_lite_load(HvPvgLite *p, const uint8_t *data, size_t len, HvPvgBytes *metal) {
    HvPvgBytes ext = {0};
    if (metal) ext = *metal, memset(metal, 0, sizeof *metal); /* taken, whatever happens */
    if (hv_pvg_lite_metal(p) || p->metal_tried) {
        fprintf(stderr, "hengevm: snapshot: the screen device is already in use\n");
        hv_pvg_bytes_release(&ext);
        return -1;
    }
    /* Into a copy first: a refused load leaves the device as it was */
    static struct HvPvgLite t; /* only its saved fields are used; too big for the stack */
    PvgParsed q;
    if (pvg_parse(data, len, ext.p, ext.len, &t, &q) != 0) {
        hv_pvg_bytes_release(&ext);
        return -1;
    }
    uint32_t status = q.status, dw = q.dw, dh = q.dh, mw = q.mw, mh = q.mh, on = q.on;
    int32_t dense = q.dense;
    HvPvgBytes keep = {0};
    if (q.mlen != UINT64_MAX && q.version == 1) {
        /* An older snapshot, with the GPU's part inside: a copy, for data goes with the reader */
        uint8_t *copy = malloc(q.mlen ? q.mlen : 1);
        if (!copy) {
            fprintf(stderr, "hengevm: snapshot: not enough memory for the screen's state\n");
            hv_pvg_bytes_release(&ext);
            return -1;
        }
        memcpy(copy, q.metal, q.mlen);
        keep = (HvPvgBytes){copy, (size_t)q.mlen, free, copy};
        hv_pvg_bytes_release(&ext);
    } else if (q.mlen != UINT64_MAX) {
        keep = ext;
    } else {
        hv_pvg_bytes_release(&ext);
    }
    memcpy(p->regs, t.regs, sizeof p->regs);
    memcpy(p->io_regs, t.io_regs, sizeof p->io_regs);
    /* The mapper's walk is derived, not saved (so v1 and v2 saves load as they are): everything submitted was
       walked when it was written, and a fence still waiting belongs to a GPU that is gone (see restored). */
    p->mapper_rp = p->mapper_fenced = t.io_regs[0x18 / 8];
    atomic_fetch_add(&p->mapper_gen, 1);
    p->mapper_more = p->mapper_unmapped = 0;
    p->root_rp = t.root_rp;
    __atomic_store_n(&p->status, status, __ATOMIC_RELEASE);
    p->chan_pending = t.chan_pending;
    p->display_pa = t.display_pa;
    p->display_logged = t.display_pa;
    p->swaps = t.swaps;
    memcpy(p->submitted, t.submitted, sizeof p->submitted);
    memset(p->waits_on, 0, sizeof p->waits_on);
    for (int i = 0; i < MAX_CHANNELS; i++) atomic_store_explicit(&p->wait_since[i], 0, memory_order_relaxed);
    p->root_waiting = 1; /* a root command may have been waiting for a barrier when saved: looked at again at the next poll */
    p->tasks = t.tasks; /* the worker's copy is made from it with the worker (ensure_metal) */
    p->draining = 0;
    display_w = dw, display_h = dh, display_mm_w = mw, display_mm_h = mh, display_set = 1;
    memcpy(offered, q.off, sizeof offered);
    offered_n = on, offered_dense = dense;
    hv_pvg_bytes_release(&p->restore_metal);
    p->restore_metal = keep;
    return 0;
}

void hv_pvg_lite_load_mapper(HvPvgLite *p, const uint8_t *data, size_t len, int version) {
    uint8_t bits[sizeof p->unmapped];
    const char *unknown = NULL;
    if (version < 0) unknown = "no PVGS";
    else if (version != HV_PVG_MAPPER_VERSION) unknown = "version";
    else if (!data || len != sizeof bits) unknown = "damaged";
    else {
        memcpy(bits, data, sizeof bits);
        if (bits[0] & 1) unknown = "damaged"; /* id 0 is none the mapper takes */
    }
    if (unknown) {
        memset(p->unmapped, 0, sizeof p->unmapped);
        fprintf(stderr, "hengevm: snapshot: mapper state: unknown (%s)\n", unknown);
        return;
    }
    memcpy(p->unmapped, bits, sizeof bits);
    unsigned n = 0;
    for (size_t i = 0; i < sizeof bits; i++) n += (unsigned)__builtin_popcount(bits[i]);
    fprintf(stderr, "hengevm: snapshot: mapper state: %u ids unmapped, restored\n", n);
}

void hv_pvg_lite_restored(HvPvgLite *p) {
    if (!p) return;
    /* A poke pending when saved isn't saved: the page says whether one is owed (a flag the guest's mask asks for and
       hasn't taken). One too many is harmless, for the guest finds the flags it reads set; one lost could leave it
       waiting. */
    const uint32_t *d = p->display_pa ? hv_phys_ptr_r(p->m, p->display_pa, 0x4000) : NULL;
    if (d && __atomic_load_n(&d[0x100 / 4], __ATOMIC_ACQUIRE) & __atomic_load_n(&d[0x104 / 4], __ATOMIC_ACQUIRE)) display_poke(p);
    if (__atomic_load_n(&p->io_regs[0x20 / 8], __ATOMIC_ACQUIRE) != p->mapper_rp) {
        /* saved while an unmap waited for the GPU: the GPU it waited for is gone, so it is done */
        __atomic_store_n(&p->io_regs[0x20 / 8], p->mapper_rp, __ATOMIC_RELEASE);
        hv_gicv3_pend(p->gic, p->iosfc_irq);
    }
    if (!p->restore_metal.p) return;
    HvPvgBytes bytes = p->restore_metal;
    memset(&p->restore_metal, 0, sizeof p->restore_metal);
    ensure_metal(p);
    HvPvgMetal *mt = hv_pvg_lite_metal(p);
    if (mt) {
        hv_pvg_metal_restore(mt, bytes); /* takes it; done before the CPUs run, and ahead of any work the guest gives it */
    } else {
        fprintf(stderr, "hengevm: snapshot: no Metal here: the screen starts blank\n");
        hv_pvg_bytes_release(&bytes);
    }
}

/* Debugging (the Mac runner's devstate): each channel's ring and stamps, to tell work never taken from work never
   finished. */
void hv_pvg_lite_debug(HvPvgLite *p, FILE *f) {
    if (!p) return;
    uint64_t root = (uint64_t)reg(p, 0x101c) << 14, buf = (uint64_t)reg(p, 0x1030) << 14;
    const uint8_t *d = p->display_pa ? hv_phys_ptr_r(p->m, p->display_pa, 0x4000) : NULL;
    if (d) fprintf(f, "devstate pvg display: +100 %x +104 %x +108 %x +10c %x +f98 %llx +fb0 %llx +fa9 %x +fbc %x +fc0 %x\n",
                   *(const uint32_t *)(d + 0x100), *(const uint32_t *)(d + 0x104), *(const uint32_t *)(d + 0x108),
                   *(const uint32_t *)(d + 0x10c), (unsigned long long)*(const uint64_t *)(d + 0xf98), (unsigned long long)*(const uint64_t *)(d + 0xfb0), d[0xfa9],
                   *(const uint32_t *)(d + 0xfbc), *(const uint32_t *)(d + 0xfc0));
    fprintf(f, "devstate pvg: vblanks %llu\n", (unsigned long long)p->vblanks);
    fprintf(f, "devstate pvg: status %x chan_pending %x draining %x stamps_pending %x root_rp %x metal %p swaps %llu\n",
            __atomic_load_n(&p->status, __ATOMIC_ACQUIRE), p->chan_pending, p->draining, p->stamps_pending, p->root_rp,
            (void *)atomic_load_explicit(&p->metal, memory_order_acquire), (unsigned long long)p->swaps);
    for (uint32_t ch = 1; root && buf && ch < MAX_CHANNELS; ch++) {
        uint32_t *rec = hv_phys_ptr_w(p->m, root + 0x400 + 0x14 * (ch - 1), 0x14);
        uint32_t *stamp = guest32_w(p, buf + 4 * ch);
        if (!rec || (!rec[0] && !rec[1] && !p->submitted[ch])) continue;
        fprintf(f, "devstate pvg ch %u: write %x read %x id %x submitted %x stamp-back %x\n", ch, rec[0], rec[1],
                rec[3], p->submitted[ch], stamp ? *stamp : 0);
    }
}
#undef PUT
#undef GET
