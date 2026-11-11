#ifndef HENGE_TL_DMG_PRIVATE_H
#define HENGE_TL_DMG_PRIVATE_H

#include "tl_dmg.h"

void tl_set_err(char *err, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* zlib (raw = no header), exactly outlen bytes. 0, or -1. */
int tl_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen, int raw);

typedef struct TlVolOps {
    const char *kind;
    int (*list)(TlVolume *vol, const TlNode *dir, TlListVisitor visit);
    /* A fork's raw bytes (0 data, 1 resource), not decompressed. */
    ssize_t (*read_fork)(TlVolume *vol, const TlNode *node, int resource, uint64_t offset, void *buf, size_t len);
    uint64_t (*fork_size)(TlVolume *vol, const TlNode *node, int resource);
    /* An extended attribute's value; returns its full length (buf may be NULL), or -1 if absent. */
    ssize_t (*get_xattr)(TlVolume *vol, const TlNode *node, const char *name, void *buf, size_t len);
    ssize_t (*readlink)(TlVolume *vol, const TlNode *node, char *buf, size_t len);
    void (*close)(TlVolume *vol);
} TlVolOps;

struct TlVolume {
    const TlVolOps *ops;
    TlDisk *disk;
    uint64_t base; /* the volume's (or container's) offset on the disk */
    char name[256];
    TlNode root;
    void *impl;
    /* The last compressed file read whole. */
    uint64_t z_id;
    uint8_t *z_data;
    uint64_t z_len;
};

int tl_vol_read(TlVolume *vol, uint64_t offset, void *buf, size_t len); /* relative to vol->base */
/* Size of a compressed file from its decmpfs header, or -1. */
int64_t tl_decmpfs_size(TlVolume *vol, const TlNode *node);

TlVolume *tl_hfs_open(TlDisk *disk, uint64_t base, char *err, size_t errlen);
TlVolume *tl_apfs_open(TlDisk *disk, uint64_t base, char *err, size_t errlen);

void tl_utf16be_to_utf8(const uint8_t *in, size_t units, char *out, size_t outlen);

#endif
