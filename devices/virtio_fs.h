#ifndef HENGEVM_VIRTIO_FS_H
#define HENGEVM_VIRTIO_FS_H

#include <pthread.h>
#include <stdint.h>

#include "virtio.h"

/* virtio-fs: a host folder the Mac mounts as a volume, the way Apple's
   Virtualization framework shares one. macOS's own driver (AppleVirtIOFS in
   com.apple.driver.AppleVirtIO) speaks Linux FUSE over the device's queues;
   this file is the other end. With the tag below macOS mounts it by itself at
   login, as "My Shared Files" in Finder's sidebar and on the desktop.

   The volume's top level is virtual: one folder per export (the iPad's shared
   folder, the read-only Henge Tools). Below that every request goes to the
   host folder, by path, never following a symbolic link on the host. */
#define HV_VFS_TAG_AUTOMOUNT "com.apple.virtio-fs.automount"
#define HV_FUSE_MAX_EXPORTS 4

typedef struct HvFuseNode HvFuseNode;
typedef struct HvFuseHandle HvFuseHandle;

typedef struct HvFuse {
    struct {
        char name[64];
        char *path;
        int readonly;
    } exports[HV_FUSE_MAX_EXPORTS];
    int nexports;
    HvFuseNode **by_id, **by_name; /* hash buckets */
    uint32_t nbuckets, nnodes;
    uint64_t next_id;
    HvFuseHandle *handles;
    uint32_t nhandles;
    uint32_t max_write;
    int64_t born;    /* the virtual top folder's times */
    int log;         /* HV_FS_LOG: every request and its answer */
    uint64_t requests, errors;
    uint32_t seen[64]; /* how often each opcode came, for the summary */
} HvFuse;

void hv_fuse_init(HvFuse *f);
/* A top-level folder named `name` that is host_dir. Returns 0, or -1 if it isn't a folder or there's no room. */
int hv_fuse_export(HvFuse *f, const char *name, const char *host_dir, int readonly);
/* One FUSE request (in, starting with its fuse_in_header) answered into out, at most out_cap bytes.
   Returns the answer's length, or 0 for requests that get none (FORGET, INTERRUPT). */
uint32_t hv_fuse_handle(HvFuse *f, const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_cap);
void hv_fuse_free(HvFuse *f);

#define HV_VFS_RING 64
#define HV_VFS_BUF (1u << 20) /* a request or answer: the 128 KB the driver may write at once, with room to spare */

typedef struct HvVfsReq {
    HvVirtqElem e;
    int q;
    uint32_t written;
    int done;
} HvVfsReq;

/* Requests run on a worker thread, as virtio-blk's do, so the CPU thread never
   waits on the host's files; the CPU thread publishes answers in order. */
typedef struct HvVirtioFs {
    HvVirtio vio;
    HvFuse fuse;
    HvVfsReq *ring;
    uint32_t submitted, completed, published;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t worker;
    uint8_t *in, *out;
} HvVirtioFs;

HvVirtioFs *hv_virtio_fs_create(HvMachine *m, HvPcie *bus, const char *tag);
/* CPU thread: publish finished requests and raise the interrupt. */
void hv_virtio_fs_poll(HvVirtioFs *d);
/* Tests: wait for the worker to finish everything submitted, then publish it. */
void hv_virtio_fs_drain(HvVirtioFs *d);

#endif
