#ifndef HENGEVM_VIRTIO_FS_INTERNAL_H
#define HENGEVM_VIRTIO_FS_INTERNAL_H

/* virtio_fs.c's own records, for Suspend (devices/vmapple_snap.c), which saves them and puts them back. */
#include "virtio_fs.h"

#define HV_FUSE_ROOT_ID 1

/* What each node ID the Mac holds stands for. A node is a name in its parent, so a rename moves a
   whole folder's worth of nodes at once. */
struct HvFuseNode {
    uint64_t id, parent;
    uint64_t nlookup;  /* how many times the Mac was told about it, less what it forgot */
    uint32_t children; /* nodes naming it as their parent: it stays while they do */
    char *name;
    int export;        /* the export it is in; -1 for the top folder */
    int linked;        /* findable by name (not removed, not replaced) */
    int pinned;        /* the top folder and the exports themselves */
    HvFuseNode *next_id, *next_name;
};

/* An open file or folder (fd -1 for folders, and for a file that could not be reopened). */
struct HvFuseHandle {
    int used, fd, dir, readonly;
    uint64_t node;
    char **names; /* a folder's listing, read at offset 0 */
    uint8_t *types;
    uint64_t *inos;
    uint32_t count;
};

/* Suspend. Loading: hv_fuse_clear, then every saved node (hv_fuse_add_node) and handle
   (hv_fuse_add_handle, hv_fuse_listing_add, hv_fuse_reopen), then hv_fuse_count_children. */
void hv_fuse_clear(HvFuse *f); /* every node and handle gone; the exports stay */
HvFuseNode *hv_fuse_node(HvFuse *f, uint64_t id);
/* A node as it was saved: 0, or -1 (no memory, or that ID is taken). Not counted in its parent yet. */
int hv_fuse_add_node(HvFuse *f, uint64_t id, uint64_t parent, const char *name, int export, uint64_t nlookup, int linked,
                     int pinned);
void hv_fuse_count_children(HvFuse *f);
/* Handle fh (1-based), in use and empty; NULL if it can't be. */
HvFuseHandle *hv_fuse_add_handle(HvFuse *f, uint64_t fh);
int hv_fuse_listing_add(HvFuseHandle *hd, uint32_t *cap, const char *name, uint8_t type, uint64_t ino);
/* Opens the handle's file again, by its node's path, O_RDONLY/O_WRONLY/O_RDWR as `access`.
   0, or a Linux error (the file is gone: the handle stays, with no file, and answers EBADF). */
int hv_fuse_reopen(HvFuse *f, HvFuseHandle *hd, int access);
/* A name the Mac may give: not empty, ".", ".." or with a "/". */
int hv_fuse_name_ok(const char *name);

#endif
