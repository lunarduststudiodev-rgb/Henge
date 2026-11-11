#ifndef HENGE_TL_DMG_H
#define HENGE_TL_DMG_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Reading a Mac disk image (.dmg) on the device, for importing an app:
   the UDIF container (raw, zero, zlib, bzip2, LZFSE, LZMA and ADC chunks),
   then the HFS+ or APFS volume inside it, read-only. Plain disk images
   without a UDIF trailer are read as they are. */

typedef struct TlDisk TlDisk;
typedef struct TlVolume TlVolume;

TlDisk *tl_disk_open(const char *path, char *err, size_t errlen);
void tl_disk_close(TlDisk *disk);
uint64_t tl_disk_size(const TlDisk *disk);
/* Bytes at an offset of the whole (decompressed) disk. 0, or -1. */
int tl_disk_read(TlDisk *disk, uint64_t offset, void *buf, size_t len);
/* The image's partitions as the image lists them (UDIF "blkx" entries). */
int tl_disk_partition_count(const TlDisk *disk);
const char *tl_disk_partition_name(const TlDisk *disk, int index);
uint64_t tl_disk_partition_offset(const TlDisk *disk, int index);
uint64_t tl_disk_partition_size(const TlDisk *disk, int index);

typedef struct TlNode {
    uint64_t id;          /* catalog id (HFS+) or inode number (APFS) */
    uint16_t mode;        /* type and permissions, as in st_mode */
    uint32_t uid, gid;
    uint32_t flags;       /* BSD flags (UF_COMPRESSED and so on) */
    uint64_t size;        /* the file's length after decompression */
    uint32_t nlink;
} TlNode;

/* The first HFS+ or APFS volume on the disk (the first APFS volume of a container). */
TlVolume *tl_volume_open(TlDisk *disk, char *err, size_t errlen);
void tl_volume_close(TlVolume *vol);
const char *tl_volume_kind(const TlVolume *vol); /* "HFS+" or "APFS" */
const char *tl_volume_name(const TlVolume *vol);
/* A path from the volume's root ("/" or "/Foo.app/Contents"), following no links. 0, or -1. */
int tl_volume_lookup(TlVolume *vol, const char *path, TlNode *out);
typedef int (^TlListVisitor)(const char *name, const TlNode *node); /* nonzero stops */
int tl_volume_list(TlVolume *vol, const TlNode *dir, TlListVisitor visit);
/* A file's contents, decompressed if the file is compressed. Returns bytes read, or -1. */
ssize_t tl_volume_read(TlVolume *vol, const TlNode *file, uint64_t offset, void *buf, size_t len);
/* A symbolic link's target. Its length, or -1. */
ssize_t tl_volume_readlink(TlVolume *vol, const TlNode *link, char *buf, size_t len);
/* Copies a file or folder (recursively) to dest, with links and permissions. 0, or -1. */
int tl_volume_extract(TlVolume *vol, const char *path, const char *dest, char *err, size_t errlen);

/* What a Mac app is: its executable's code and the oldest macOS it allows. */
typedef struct TlAppInfo {
    char name[256], identifier[256], executable[256], min_macos[32];
    int has_arm64, has_arm64e, has_x86_64;
    char build_min_macos[32]; /* from the arm64 slice's LC_BUILD_VERSION, if any */
} TlAppInfo;
/* path: an extracted .app folder, or a Mach-O file. 0, or -1. */
int tl_app_info(const char *path, TlAppInfo *info, char *err, size_t errlen);

#endif
