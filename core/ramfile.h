#ifndef HENGEVM_RAMFILE_H
#define HENGEVM_RAMFILE_H

#include <stddef.h>
#include <stdint.h>

/* The Mac's memory as files kept current on storage, so a Suspend only writes what changed
   (the save ahead design notes).

   The memory is ordinary memory (outside the app's footprint, as the Mac's memory without Quick Suspend is), and
   the files in a run folder (128 MB each) a copy of it kept close behind. Nothing maps the files: a Mac whose
   memory was a file mapping waited on storage (iOS lets go of file pages first, and reads them back as the Mac
   touches them; a page being written back can't be written). Instead one byte per page says whether the files may
   not hold it (the dirty map, HV_DIRTY_* in core/machine.h): the engine marks a page as a CPU first writes it after
   it was last written (a write needs a TLB entry, and the writer drops those for each page it takes), and as a
   device writes it (hv_dma_written, or a pointer to it handed out: marked lent). A background writer copies dirty
   pages out and writes them (pwrite) to keep that set small.

   A save writes the rest with the Mac stopped, then clones the run folder (APFS: no data copied) into a saved
   copy beside the snapshot, <snapshot>.ram-<generation>, that nothing ever writes again. The snapshot records the
   generation and holds, itself, every page the copy may not have right: those still dirty and those under a GPU
   mapping (what the GPU writes isn't seen as a write). A resume clones the
   saved copy into the run folder again, so a resume that fails or crashes part way leaves the save as it was.

   Before a save is used it is checked against the memory itself: sampled 1 MB pieces of the saved copy, read
   back from storage with the snapshot's pages laid over them, must equal the memory (else the save is whole),
   and the same pieces are checked again after a resume (else it is refused). */

typedef struct HvRamFile HvRamFile;
/* The dirty map's values, per page */
#define HV_DIRTY_CLEAN 0 /* the files hold it, and nothing can write it unmarked */
#define HV_DIRTY 1       /* changed since the files last got it */
#define HV_DIRTY_LENT 2  /* a device has a pointer it may write through (hv_phys_ptr): dirty until it says it is done */
/* Drops every writable TLB entry to these pages (ascending page numbers), so the next store to each marks it dirty
   (core/machine.c sets it: hv_machine_attach_file). Called by a writer, never with the Mac's device lock held. */
typedef void (*HvRamFileRevoke)(void *ctx, HvRamFile *f, const uint32_t *pages, uint32_t n);

#define HV_RAMFILE_GEN 16
#define HV_RAMFILE_CHECKS 64
#define HV_RAMFILE_CHECK_BYTES (1u << 20)

/* A new run folder at dir (anything there before removed: no save refers to it) holding size bytes of zeros, and
   memory of zeros to match. NULL on failure (logged): then nothing is left mapped. */
HvRamFile *hv_ramfile_open(const char *dir, uint64_t size);
/* Unmaps and removes the run folder (tests; a process that exits leaves it for the next open). */
void hv_ramfile_close(HvRamFile *f);
uint8_t *hv_ramfile_base(const HvRamFile *f);
uint64_t hv_ramfile_size(const HvRamFile *f);
/* The system's page size the memory is tracked in (16 KB). */
uint64_t hv_ramfile_page(const HvRamFile *f);
/* The dirty map: one byte per page (HV_DIRTY_*). Whatever writes the memory other than through the engine marks
   what it wrote (hv_ramfile_mark_dirty). */
uint8_t *hv_ramfile_dirty_map(HvRamFile *f);
void hv_ramfile_set_revoke(HvRamFile *f, HvRamFileRevoke fn, void *ctx);
/* [off, off + len) written (marked dirty, after a fence: the writes before this are seen by the writer that takes the
   mark). */
void hv_ramfile_mark_dirty(HvRamFile *f, uint64_t off, uint64_t len);
/* The whole pages in [off, off + len) given back to the system (the balloon): zeros from now on, marked dirty (so the
   files get the zeros, as holes). Pages under a GPU mapping are zeroed in place instead. 0, or -1. Fresh memory,
   never MADV_FREE_REUSABLE: the balloon's take-back counts on that (it does no MADV_FREE_REUSE for these). */
int hv_ramfile_discard(HvRamFile *f, uint64_t off, uint64_t len);
/* The give-backs since the last line about them (logged at most once a minute): the longest time one held the lock
   the GPU's mappings take, and how many. Read by the tests; for the memory log's line once guest-memory's is merged
   (nothing there reads it yet). */
void hv_ramfile_hold_stats(uint64_t *max_ns, uint64_t *n);

/* Back to all zeros (new files), at the same address. Nothing may be using the memory: no CPU, device or GPU
   mapping. 0, or -1 (then the memory must not be used). */
int hv_ramfile_reset(HvRamFile *f);
/* A Mac starting afresh where a resume put a saved copy (and was then refused or failed): zeros first. 0, or -1. */
int hv_ramfile_make_fresh(HvRamFile *f);

/* What a save records, beside the pages it holds itself. */
typedef struct HvRamFileCheck {
    uint32_t chunk; /* which HV_RAMFILE_CHECK_BYTES piece */
    uint32_t crc;   /* CRC32C of the memory there */
} HvRamFileCheck;
typedef struct HvRamFileSave {
    uint8_t gen[HV_RAMFILE_GEN];
    uint32_t *pages; /* pages the saved copy may not have right, ascending (malloc'd: hv_ramfile_save_free) */
    uint64_t npages;
    uint32_t nchecks;
    HvRamFileCheck check[HV_RAMFILE_CHECKS];
    /* An auto-save (hv_ramfile_take): page k of pages is at copy + slot[k] * page, the memory as it was at the instant
       saved (the save guard's buffer, core/saveguard.h, which outlives the save). copy NULL: they are in the memory. */
    const uint8_t *copy;
    uint32_t *slot; /* malloc'd, freed with the save */
    /* ...and the pieces hv_ramfile_finish checks the copy against, their CRCs taken at that instant (malloc'd, freed
       with the save; check[] holds the first HV_RAMFILE_CHECKS of them, for a resume). */
    HvRamFileCheck *verify;
    uint32_t nverify;
} HvRamFileSave;
void hv_ramfile_save_free(HvRamFileSave *s);

/* The save (the Mac stopped: CPUs, devices, GPU). Every modified page written, the run folder cloned into
   <snap>.ram-<generation> and that on storage (F_FULLFSYNC), the pages it may not have listed, and the sampled
   check passed. Stopped part way once *cancel != ticket (cancel may be NULL): -3. 1: no saved copy can be trusted
   this time (logged): save the memory whole. -1: failed (logged). On anything but 0 no saved copy is left. */
int hv_ramfile_commit(HvRamFile *f, const char *snap, int threads, const uint64_t *cancel, uint64_t ticket, HvRamFileSave *out);
/* An auto-save: the memory as it was at one instant (T), saved while the Mac runs on (the autosave design notes, v3).
   The save guard (core/saveguard.h) keeps T: from T on, the first write to a page copies it first ("claims" it).

   Before T, the Mac running: hv_ramfile_catch_up writes what changed until little is left (what is still dirty
   at T is claimed, below: copied); hv_ramfile_prepare picks the generation and the pieces the save is checked with,
   and brings them in from storage (so taking their CRCs at T reads no storage).

   At T (CPUs, devices and GPU stopped; the guard armed): hv_ramfile_mark takes the pieces' CRCs: the memory at T,
   which the finish holds the saved copy to (and a resume the memory). A few milliseconds for 64 MB.

   After T, the Mac running, the guard still armed, claim(ctx, off, len) claiming [off, off + len) of the memory:
   - hv_ramfile_claim_gpu, before the GPU runs again (what it writes, no claim sees): every page under a GPU mapping,
     and from then on until hv_ramfile_claim_stop, every page a new GPU mapping covers as it is made.
   - hv_ramfile_take: claims every page the files may not hold (dirty), puts the run files on storage (writes under
     way finished) and clones them into <snap>.ram-<gen>. Every page not claimed is then T's in the copy.
   - hv_ramfile_claim_stop, then the guard disarmed; hv_ramfile_claimed lists the pages claimed in this memory (guard
     region `region`) and where each one's T bytes are in the guard's buffer.
   Then the snapshot (with those pages) and hv_ramfile_finish: the copy on storage, its pieces read back with the
   claimed pages over them and checked against the CRCs of T.
   Each: 0; 1: no saved copy can be trusted this time (logged; save whole, or not at all); -1: failed; -3:
   cancelled (*cancel != ticket). On anything but 0 from take, claimed or finish, the copy is removed and the save
   freed. */
typedef void (*HvRamFileClaim)(void *ctx, uint64_t off, uint64_t len);
uint64_t hv_ramfile_catch_up(HvRamFile *f, uint64_t under, int max_passes, uint64_t max_ns, int threads, const uint64_t *cancel,
                             uint64_t ticket);
int hv_ramfile_prepare(HvRamFile *f, HvRamFileSave *out);
int hv_ramfile_mark(HvRamFile *f, int threads, HvRamFileSave *s);
uint64_t hv_ramfile_claim_gpu(HvRamFile *f, HvRamFileClaim claim, void *ctx);
int hv_ramfile_take(HvRamFile *f, const char *snap, int threads, const uint64_t *cancel, uint64_t ticket, HvRamFileSave *s);
void hv_ramfile_claim_stop(HvRamFile *f);
int hv_ramfile_claimed(HvRamFile *f, const char *snap, HvRamFileSave *s, const uint64_t *slot_page, uint64_t nslots, unsigned region,
                       const uint8_t *buf);
int hv_ramfile_finish(HvRamFile *f, const char *snap, int threads, const uint64_t *cancel, uint64_t ticket, HvRamFileSave *s);
/* Whether <snap>.ram-<gen> is a whole saved copy of this size. 0, or -1 with why (a sentence for the person). */
int hv_ramfile_check_saved(const char *snap, const uint8_t gen[HV_RAMFILE_GEN], uint64_t size, char *why, size_t why_len);
/* The saved copy becomes the memory (a resume, before anything uses the memory): cloned into the run folder, at the
   same address, and read into the memory whole (on up to 8 threads). The copy itself is left as it was. 0, or -1 with
   why (then the memory must not be used). */
int hv_ramfile_adopt(HvRamFile *f, const char *snap, const uint8_t gen[HV_RAMFILE_GEN], char *why, size_t why_len);
/* After a resume laid the snapshot's pages over the copy: the sampled pieces equal what was saved. 0, or -1. */
int hv_ramfile_verify(HvRamFile *f, const HvRamFileCheck *check, uint32_t n);
/* Deletes the saved copies of <snap> (all of them, or all but keep's), each on storage before this returns. 0, or -1. */
int hv_ramfile_remove_saved(const char *snap, const uint8_t *keep);
/* Deletes the one saved copy <snap>.ram-<gen> (a save that didn't finish). 0, or -1. */
int hv_ramfile_remove_copy(const char *snap, const uint8_t gen[HV_RAMFILE_GEN]);

/* GPU mappings over the memory (devices/pvg_metal.m). The pages under one may be wired, and what the GPU writes
   there isn't seen as a write: while mapped, and until the next save after, they always go into a save; once
   unmapped they are marked dirty (so the writer or the next save writes them), and again when a save stops
   taking them. src: each page's address in the memory (an entry NULL: none; addresses outside every open memory
   are ignored), mapping: the key unmap is called with (an unmap takes the oldest mapping of that key: exactly one
   unmap for each map, before the mapping's addresses can be reused). src NULL (its pages couldn't be listed): every later save of this
   run is whole. */
void hv_ramfile_gpu_map(const void *mapping, void *const *src, size_t n);
void hv_ramfile_gpu_unmap(const void *mapping);
/* The oldest mapping of <mapping> lists src instead (pages it no longer holds cleared or left out), in one step: no
   page kept is unpinned meanwhile. No mapping of that key: nothing. src NULL (or no memory): the old list stays. */
void hv_ramfile_gpu_relist(const void *mapping, void *const *src, size_t n);
/* Pages a save takes for the GPU now: under a mapping, or under one since the last save (the log and tests). */
/* 0 once a GPU mapping went untracked this run: hv_ramfile_commit then always says to save the memory whole. */
int hv_ramfile_copyable(void);
uint64_t hv_ramfile_gpu_pages(HvRamFile *f);

/* The background writer: started once the Mac runs, held while it is paused (a save writes then).
   hold waits for a write under way to stop, so the save that follows has the storage to itself. Never hold (nor
   sync, catch up or take) with the Mac's device lock held: a pass under way may be taking back writes, which waits
   for every running CPU, and one may be waiting for that lock. */
void hv_ramfile_writer_start(HvRamFile *f);
void hv_ramfile_writer_hold(HvRamFile *f, int hold);

/* Every dirty page written, the files flushed (no copy made; the writer held meanwhile). 0, -1, or -3 cancelled. */
int hv_ramfile_sync(HvRamFile *f, int threads, const uint64_t *cancel, uint64_t ticket);
/* Pages still dirty (what the files may not hold): page numbers, ascending, in a malloc'd array the
   caller frees. 0, or -1. */
int hv_ramfile_modified(HvRamFile *f, uint32_t **pages, uint64_t *n);
/* Storage a save would take now, at most (the Mac paused): every modified page written into the files (new blocks,
   as a saved copy may hold the old ones) and saved beside them again, with the pages under the GPU. The saved copy
   itself shares the files' blocks. A save that ends up whole (the check failed) needs the whole memory instead. */
uint64_t hv_ramfile_save_need(HvRamFile *f);
/* Bytes the run folder may still grow by on storage (its size less what it has allocated). */
uint64_t hv_ramfile_unallocated(const HvRamFile *f);
/* Puts len bytes at off of the memory, marked dirty (a resume laying a save's pages over the copy). 0, or -1. */
int hv_ramfile_put(HvRamFile *f, uint64_t off, const void *src, size_t len);

/* Background writer accounting (for the log and tests). */
typedef struct HvRamFileStats {
    uint64_t written, flushes, scans, last_dirty, high, low, rate;
} HvRamFileStats;
void hv_ramfile_stats(HvRamFile *f, HvRamFileStats *s);
/* Tests: the writer's policy (high 0: from the measured rate), and one pass of it now (returns bytes written). */
void hv_ramfile_set_policy(HvRamFile *f, uint64_t high_bytes, double bytes_per_s, uint64_t burst_bytes, unsigned period_ms);
uint64_t hv_ramfile_writer_pass(HvRamFile *f);
/* Tests: every piece checked at a save, not a sample (as HV_RAM_VERIFY does). */
void hv_ramfile_check_all(HvRamFile *f, int all);
/* Tests: the saved copy gets this page wrong (as a change nobody marked would leave it); -1 for none. */
void hv_ramfile_test_spoil(HvRamFile *f, int64_t page);

#endif
