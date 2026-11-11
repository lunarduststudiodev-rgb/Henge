#ifndef HENGEVM_SNAPSHOT_H
#define HENGEVM_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

/* Instant resume: the whole machine (CPUs, RAM, devices, the screen) in one file, and back.

   Saving needs a machine paused with hv_vm_pause (every CPU out of guest code, devices idle, the GPU's own memory
   copied back into the guest's). Restoring goes into a machine just made by hv_vm_create with the same settings, before
   hv_vm_start. ident is the caller's own description of what the file is valid for (the app's build, the Mac, its disk
   as it was when saved); a file made under another ident, or by another build of the engine, is refused.

   The disk is not in the file: it has to be exactly as it was when the file was written, so a file must be resumed at
   most once, and never after the Mac has run on without it. */
int hv_snapshot_save(const char *path, const char *ident, char *err, size_t errlen);
int hv_snapshot_restore(const char *path, const char *ident, char *err, size_t errlen);
/* 0 if path holds a complete snapshot made with ident by this build; its size in bytes and when it was made. */
int hv_snapshot_check(const char *path, const char *ident, uint64_t *bytes, int64_t *saved_at);

#endif
