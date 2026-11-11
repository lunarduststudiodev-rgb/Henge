#ifndef HENGEVM_DISK_H
#define HENGEVM_DISK_H

#include <stdint.h>

/* A raw disk image file, optionally starting `offset` bytes in (QEMU's vmapple
   notes say the first 16 KB of auxiliary.bin is metadata the guest never sees). */
typedef struct HvDisk {
    int fd;
    int wfd;         /* writes, past the host's cache (F_NOCACHE); -1: through fd */
    uint64_t offset;
    uint64_t size;   /* bytes visible to the guest */
    int readonly;
    struct HvDiskGuard *guard; /* an auto-save's, while armed (hv_disk_guard_arm) */
    uint32_t guard_users;      /* writers that may hold `guard` (atomic): its end waits for none */
} HvDisk;

int hv_disk_open(HvDisk *d, const char *path, uint64_t offset, int readonly);
void hv_disk_close(HvDisk *d);
/* Return 0, or -1 on an I/O error or an access past the end. */
int hv_disk_read(HvDisk *d, uint64_t off, void *buf, uint64_t len);
int hv_disk_write(HvDisk *d, uint64_t off, const void *buf, uint64_t len);
int hv_disk_flush(HvDisk *d);
/* Everything written so far onto the storage itself (F_FULLFSYNC), whatever HV_FLUSH says: what
   survives the device losing power or restarting. 0, or -1. */
int hv_disk_sync(HvDisk *d);
/* A copy of the whole image file (offset part included) at `to`, as it is now: an APFS clone, so nothing is copied
   and later writes to either leave the other alone. Writes the guest started must have finished (CPUs paused, the
   device idle). The clone and its directory entry are on the storage itself when it returns 0; on -1 nothing is
   left at `to`. id_* (any may be NULL): the clone's inode, size and modification time (ns), as host/vm.c's
   DiskId reads them from an open disk. */
int hv_disk_clone(HvDisk *d, const char *to, uint64_t *id_ino, uint64_t *id_size, int64_t *id_mtime_ns);
/* The open disk on the file at `path` instead (its offset and read-only as before): the same size, or -1 and nothing
   changes. For a disk nothing has been written to since it was opened (a restore, before the Mac runs). */
int hv_disk_reopen(HvDisk *d, const char *path);
/* The open image put in `to`'s place as a file of its own (an APFS clone), and the disk then that file
   (hv_disk_reopen). What was at `to` is moved to `aside`, never over anything there ("<aside stem> 2.img" and on if
   it is taken), unless `aside` is NULL or it is the same disk as the open one (the same size and modification time:
   nothing written to it since): then it goes. If it can't be kept aside it stays at `<to>.placing`, never deleted.
   `to` holds a whole disk at every instant (the two are swapped); a start that ended partway leaves `<to>.placing`,
   which the next call keeps aside first (or deletes, if it is a copy of the open disk). Both directories end on
   storage. 0, or -1. */
int hv_disk_place(HvDisk *d, const char *to, const char *aside);

/* An auto-save's disk guard: the disk as it was at one instant (T), kept while the Mac writes on.

   Armed in the stop (the CPUs paused, no disk request under way), the first write to each block after it first
   copies the block as it was into a log (a file in the save's folder, unlinked at once). The copy of the disk is
   made after the Mac runs again (hv_disk_clone_guarded): an APFS clone of the image as it is then, which has T's
   bytes in every block not written since and may have later ones in the blocks the log holds; the log's blocks are
   then written over the clone, which is then the disk at T. A writer never waits for room: when the log is full
   (or can't be written) the save is void and the write goes on. */
#define HV_DISK_GUARD_BLOCK (16u << 10)
typedef struct HvDiskGuard HvDiskGuard;
/* A guard for d with a log of at most max_bytes in dir (less where storage, less reserve bytes, is short). NULL
   if it can't be made. */
HvDiskGuard *hv_disk_guard_new(HvDisk *d, const char *dir, uint64_t max_bytes, uint64_t reserve);
/* In the stop: no request under way, none can start. */
void hv_disk_guard_arm(HvDisk *d, HvDiskGuard *g);
/* After the stop, the Mac running: what the disk was at the arm, at `to` (as hv_disk_clone, and id_* read after
   the log was laid over it). The guard is ended either way. 0; -1 failed (nothing at `to`); 1 void: the log was
   full or could not be written (nothing at `to`). */
int hv_disk_clone_guarded(HvDisk *d, const char *to, uint64_t *id_ino, uint64_t *id_size, int64_t *id_mtime_ns);
/* Ends the guard if still armed (a save given up), and frees it. */
void hv_disk_guard_free(HvDisk *d, HvDiskGuard *g);
/* Statistics of the last guard: blocks logged. */
uint64_t hv_disk_guard_logged(const HvDiskGuard *g);
int hv_disk_guard_test_logged(const HvDiskGuard *g, uint64_t block); /* tests */
int hv_disk_guard_test_entry(const HvDiskGuard *g, uint64_t block, void *to);

#endif
