#ifndef HENGEVM_SNAPSHOT_H
#define HENGEVM_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

#include "machine.h"

/* Suspend: a paused machine's whole state in one file, loaded later into a new machine (in a new
   process too) that carries on as if it had never stopped.

   All or nothing. The file is written as <path>.partial and renamed to <path> only once everything,
   and then the header, is on storage; a crash or a full disk at any moment leaves the previous file
   (or none), never a half one. The header lists every part with its length and CRC32C, and loading
   checks each before using it: a damaged file is refused, not half loaded.

   Layout: the header (64 KB), then any streamed sections, then guest RAM (from a 64 KB boundary), then
   the other sections.
   - Streamed sections: big ones (what the GPU holds) written to the file as they are made, never whole in
     memory; they must all come before the RAM. One CRC each, like the others.
   - RAM: each region in 1 MB slots at fixed offsets, so threads write chunks in any order. A slot
     holds its chunk LZ4-compressed, raw if that would not save much, or nothing if it is all zeros;
     the rest of the slot is a hole in the file. Each chunk's CRC is of its uncompressed bytes.
   - Sections: small, tagged and versioned (CPUs, clock, GIC, devices), written field by field, never
     as a copy of a struct, so a field moving in memory never silently changes the file. */

#define HV_SNAP_FORMAT 1
#define HV_SNAP_TAG(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define HV_SNAP_CPUS HV_SNAP_TAG('C', 'P', 'U', 'S')
#define HV_SNAP_TIME HV_SNAP_TAG('T', 'I', 'M', 'E')
#define HV_SNAP_RAM  HV_SNAP_TAG('R', 'A', 'M', ' ')
#define HV_SNAP_GIC  HV_SNAP_TAG('G', 'I', 'C', '3')

#define HV_SNAP_NO_SYNC 1u   /* hv_snap_begin: skip the flushes to storage (tests only) */
#define HV_SNAP_DIRTY_RAM 2u /* hv_snap_get_ram: the RAM is not freshly zeroed: write the zero chunks too */
#define HV_SNAP_NO_WHOLE 4u  /* hv_snap_begin: a region kept in files is never saved whole: hv_snap_put_ram fails instead */

typedef struct HvSnapWriter HvSnapWriter;
typedef struct HvSnapReader HvSnapReader;

/* Sections are built and read field by field (HV_SNAP_OUT / HV_SNAP_IN). Reading past the end, or not
   reading to it, marks the section bad. */
typedef struct HvSnapOut {
    uint8_t *p;
    size_t len, cap;
    int bad;
} HvSnapOut;
typedef struct HvSnapIn {
    const uint8_t *p;
    size_t len, at;
    int bad; /* 1: damaged; HV_SNAP_REFUSED: a loader refused it and already said why */
} HvSnapIn;
#define HV_SNAP_REFUSED 2
void hv_snap_out(HvSnapOut *o, const void *v, size_t n);
void hv_snap_in(HvSnapIn *in, void *v, size_t n);
#define HV_SNAP_OUT(o, field) hv_snap_out((o), &(field), sizeof(field))
#define HV_SNAP_IN(in, field) hv_snap_in((in), &(field), sizeof(field))

/* Writing. Errors are logged on stderr ("hengevm: snapshot: ..."); once one call fails the rest do
   nothing and commit fails. */
HvSnapWriter *hv_snap_begin(const char *path, unsigned flags);
/* Stops a save part way: once *count is no longer ticket, the RAM's write stops at its next chunk and fails
   ("cancelled"); the caller then aborts. count NULL: never (the default). */
void hv_snap_set_cancel(HvSnapWriter *w, const uint64_t *count, uint64_t ticket);
/* 1 if the save was stopped that way */
int hv_snap_cancelled(HvSnapWriter *w);
/* Adds a section; o is consumed (freed) either way. */
int hv_snap_put(HvSnapWriter *w, uint32_t tag, uint32_t version, HvSnapOut *o);
/* Every RAM region of m, on `threads` threads. The machine must be paused. */
int hv_snap_put_ram(HvSnapWriter *w, HvMachine *m, int threads);
/* An auto-save's RAM (the autosave design notes, v3): the memory as it was at the instant saved, while the Mac runs
   on. A region kept in its files has the auto-save's copy and claimed pages (core/ramfile.h, through
   hv_ramfile_claimed: finished here, so the copy is checked before the snapshot names it); every other writable
   region its bytes at that instant (copied while the Mac was stopped). The save is consumed (freed) either way. */
struct HvRamFileSave;
typedef struct HvSnapTaken {
    struct HvRamFileSave *file[HV_MAX_RAM];
    const uint8_t *host[HV_MAX_RAM];
} HvSnapTaken;
int hv_snap_put_ram_taken(HvSnapWriter *w, HvMachine *m, int threads, HvSnapTaken *t);
/* A section too big to build in memory (the screen's GPU state can be gigabytes), written to the file as it
   is made: begin, write any number of times, end. Streamed sections go before the RAM (hv_snap_put_ram after
   them), one at a time. end(keep = 0) drops it (its bytes are left in the file as junk nothing points at);
   end returns -1 if anything failed, and then the save fails. write and end may be called on another thread
   than the writer's others, but never at the same time as them. */
typedef struct HvSnapStream HvSnapStream;
HvSnapStream *hv_snap_stream_begin(HvSnapWriter *w, uint32_t tag, uint32_t version);
int hv_snap_stream_write(HvSnapStream *s, const void *p, size_t n);
int hv_snap_stream_end(HvSnapStream *s, int keep);
/* Free bytes on the storage the snapshot is written to; UINT64_MAX if that can't be told. */
uint64_t hv_snap_free_bytes(HvSnapWriter *w);
/* Finishes the file and frees w: 0 once <path> is the complete snapshot. On failure <path> is as it was.
   Called off (hv_snap_set_cancel) up to the header's write: fails, keeping nothing. */
int hv_snap_commit(HvSnapWriter *w);
void hv_snap_abort(HvSnapWriter *w);

/* Reading. hv_snap_open checks the header and every section; why (may be NULL) says what was wrong. */
HvSnapReader *hv_snap_open(const char *path, char *why, size_t why_len);
/* A section, which must have this version: 0 and *in set, or -1 (missing or another version). */
int hv_snap_get(HvSnapReader *r, uint32_t tag, uint32_t version, HvSnapIn *in);
/* 0 if `in` was read exactly to its end with nothing wrong. */
int hv_snap_in_done(HvSnapIn *in);
/* RAM into m, whose regions must be the saved ones (same addresses and sizes). Read-only regions are
   not written but checked against the snapshot. Any bad chunk fails it: the machine must then be
   thrown away, not run. */
int hv_snap_get_ram(HvSnapReader *r, HvMachine *m, int threads, unsigned flags);
/* Whether hv_snap_get_ram could load into m, nothing changed: the regions, and for memory kept in its file
   (core/ramfile.h), that the file is the one saved with. 0, or -1 with why (a sentence for the person). */
int hv_snap_check_ram(HvSnapReader *r, HvMachine *m, char *why, size_t why_len);
/* A section's bytes, kept after the reader is closed: big ones are mapped from the file, not in memory. */
typedef struct HvSnapBuf {
    const uint8_t *p;
    size_t len;
    void *base;
    size_t map_len;
    int mapped;
} HvSnapBuf;
/* Takes a section's bytes from the reader (the caller frees them with hv_snap_buf_free): 0, or -1 as
   hv_snap_get. After this hv_snap_get of the same tag fails. */
int hv_snap_take(HvSnapReader *r, uint32_t tag, uint32_t version, HvSnapBuf *b);
/* 1 if the file has a section of this tag (of any version), else 0; says nothing either way. */
int hv_snap_has(HvSnapReader *r, uint32_t tag);
/* The version of the file's section of this tag, or -1 if it has none; says nothing. */
int hv_snap_version(HvSnapReader *r, uint32_t tag);
void hv_snap_buf_free(HvSnapBuf *b);
void hv_snap_close(HvSnapReader *r);
/* Deletes the snapshot at path (and a save of it that never finished), the deletion on storage before it
   returns: once a machine runs on from a snapshot, that snapshot is out of date and must never be loaded.
   0 when there is none any more (or there was none), -1 if it could not be deleted. (With no snapshot there,
   only an unfinished save, nothing is flushed: an unfinished save is never loaded.) */
int hv_snap_discard(const char *path);

/* The CPUs and the guest's clock. Saving needs the machine paused (hv_machine_pause_wait). Loading
   is into a machine whose CPUs exist (as many as were saved) and are not running; it is left paused,
   its counter where the saved one stopped, and hv_machine_resume lets it go once the CPU threads run. */
int hv_snap_put_cpus(HvSnapWriter *w, HvMachine *m);
int hv_snap_get_cpus(HvSnapReader *r, HvMachine *m);
/* The experimental Rosetta mode the CPUs were saved in (0 off, 1, 2), which hv_snap_get_cpus puts the machine in;
   -1 if the CPU section is missing, of another version or damaged. Says nothing. */
int hv_snap_cpus_rosetta(HvSnapReader *r);

uint32_t hv_crc32c(uint32_t crc, const void *data, size_t len);

#endif
