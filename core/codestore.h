#ifndef HENGEVM_CODESTORE_H
#define HENGEVM_CODESTORE_H

#include <stdint.h>

#include "machine.h"
#include "snapshot.h"

/* The code store: a suspended Mac's translated code, kept beside its snapshot (<snapshot>.code), so that the Mac
   resumed from it doesn't translate everything it was running all over again (seconds of slow going, and more each
   time the code cache had filled).

   What is saved is the code cache as it is: each zone's code words, its block records, its chainable exits and its
   FP/SIMD word records, at the same places in the same tables. A resumed Mac has the same guest memory at the same
   addresses, so a block saved live is still right; only the host's side moves (where the code region, the RAM and
   the slot table are in the new process), and the store is fixed up for that as it loads:
   - every exit is unchained (a chained B back to its stub, and its conditional branch with it), and every slot
     starts empty, as the dispatcher's lists of chained exits do;
   - each stub's inline pointer to its slot, and each exit to another page's expected TLB key (which holds the host
     address of the target page's RAM) are rewritten, after checking each is what the translator writes there;
   - blocks whose code holds other host addresses (clock reads, IC IVAU: HvBlock.nosave) were never saved: they
     are translated again when first run.
   The code is loaded at the same offsets in the region, so every branch inside it still lands where it did.

   Never in the way: a store is only a head start. It is loaded only when everything it depends on is the same (the
   build of Henge, the translator's settings, the code cache's size and layout, the RAM's) and its key matches the
   snapshot's (one store per Suspend: a store left from another is ignored). Each zone is checked (CRC32C) and
   loaded on its own; any problem drops that zone, and the Mac runs on with what is left, as it would with none.
   Saving fails without a trace if there is no room for it; the snapshot never depends on it. */

typedef struct HvCodeKey {
    uint8_t id[16];
} HvCodeKey;

#define HV_SNAP_CODE_KEY HV_SNAP_TAG('J', 'I', 'T', 'K')

/* A fresh key; and the snapshot's section that names the store saved with it (older builds ignore it). */
void hv_code_key_new(HvCodeKey *k);
int hv_code_key_put(HvSnapWriter *w, const HvCodeKey *k);
/* 0 and *k, or -1 if the snapshot names no store. */
int hv_code_key_get(HvSnapReader *r, HvCodeKey *k);

/* The paused machine's code cache to <snap_path>.code (written aside and renamed into place; no flush to storage
   is waited for: a store damaged by a crash fails its checks and is ignored). Stops, leaving nothing, once *cancel
   is no longer ticket (cancel NULL: never). 0, or -1 with nothing kept (and the reason logged). */
int hv_code_store_save(HvMachine *m, const char *snap_path, const HvCodeKey *k, const uint64_t *cancel,
                       uint64_t ticket);
/* Into a machine restored from that snapshot, before any CPU runs, its code cache empty: the blocks loaded (0 if
   none could be), or -1 if the store isn't there or isn't for this machine. Says what it did on stderr. */
long hv_code_store_load(HvMachine *m, const char *snap_path, const HvCodeKey *k);
/* Deletes <snap_path>.code (and a save of it that never finished). */
void hv_code_store_remove(const char *snap_path);

#endif
