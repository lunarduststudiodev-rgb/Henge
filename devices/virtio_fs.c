#include "virtio_fs.h"
#include "virtio_fs_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

/* ---- The FUSE protocol, as Linux defines it (include/uapi/linux/fuse.h, 7.31).
   macOS's driver uses exactly these layouts and Linux's error numbers and open
   flags (AppleVirtIOFS_FuseOps::convertError and ::convertOpenFlags translate);
   the sizes below are the ones its requests were seen to use. */

enum {
    OP_LOOKUP = 1, OP_FORGET = 2, OP_GETATTR = 3, OP_SETATTR = 4, OP_READLINK = 5, OP_SYMLINK = 6, OP_MKNOD = 8,
    OP_MKDIR = 9, OP_UNLINK = 10, OP_RMDIR = 11, OP_RENAME = 12, OP_LINK = 13, OP_OPEN = 14, OP_READ = 15,
    OP_WRITE = 16, OP_STATFS = 17, OP_RELEASE = 18, OP_FSYNC = 20, OP_SETXATTR = 21, OP_GETXATTR = 22,
    OP_LISTXATTR = 23, OP_REMOVEXATTR = 24, OP_FLUSH = 25, OP_INIT = 26, OP_OPENDIR = 27, OP_READDIR = 28,
    OP_RELEASEDIR = 29, OP_FSYNCDIR = 30, OP_ACCESS = 34, OP_CREATE = 35, OP_INTERRUPT = 36, OP_DESTROY = 38,
    OP_BATCH_FORGET = 42, OP_RENAME2 = 45, OP_LSEEK = 46,
};

/* Linux error numbers: the driver turns them into macOS's. */
enum {
    L_EPERM = 1, L_ENOENT = 2, L_EIO = 5, L_EBADF = 9, L_EAGAIN = 11, L_ENOMEM = 12, L_EACCES = 13, L_EEXIST = 17,
    L_EXDEV = 18, L_ENOTDIR = 20, L_EISDIR = 21, L_EINVAL = 22, L_ENOSPC = 28, L_EROFS = 30, L_ERANGE = 34,
    L_ENAMETOOLONG = 36, L_ENOSYS = 38, L_ENOTEMPTY = 39, L_ELOOP = 40, L_ENODATA = 61, L_EOPNOTSUPP = 95,
    L_EPROTO = 71, L_ETIMEDOUT = 110, L_EDQUOT = 122,
};

#define L_O_ACCMODE 03
#define L_O_WRONLY 01
#define L_O_CREAT 0100
#define L_O_EXCL 0200
#define L_O_TRUNC 01000

#define FATTR_MODE (1u << 0)
#define FATTR_SIZE (1u << 3)
#define FATTR_ATIME (1u << 4)
#define FATTR_MTIME (1u << 5)
#define FATTR_FH (1u << 6)
#define FATTR_ATIME_NOW (1u << 7)
#define FATTR_MTIME_NOW (1u << 8)

#define FUSE_ASYNC_READ (1u << 0)
#define FUSE_ATOMIC_O_TRUNC (1u << 3)
#define FUSE_BIG_WRITES (1u << 5)
#define FUSE_MAX_PAGES (1u << 22)
#define FUSE_GETATTR_FH (1u << 0)
#define RENAME_NOREPLACE_L (1u << 0)
#define RENAME_EXCHANGE_L (1u << 1)

#define ROOT_ID 1
#define VALID_S 1 /* how long the Mac may trust a name or attributes: the iPad can change files too */

struct in_hdr { uint32_t len, opcode; uint64_t unique, nodeid; uint32_t uid, gid, pid; uint16_t extlen, padding; };
struct out_hdr { uint32_t len; int32_t error; uint64_t unique; };
struct fattr {
    uint64_t ino, size, blocks, atime, mtime, ctime;
    uint32_t atimensec, mtimensec, ctimensec, mode, nlink, uid, gid, rdev, blksize, flags;
};
struct entry_out { uint64_t nodeid, generation, entry_valid, attr_valid; uint32_t entry_valid_nsec, attr_valid_nsec; struct fattr attr; };
struct attr_out { uint64_t attr_valid; uint32_t attr_valid_nsec, dummy; struct fattr attr; };
struct getattr_in { uint32_t flags, dummy; uint64_t fh; };
struct setattr_in {
    uint32_t valid, padding;
    uint64_t fh, size, lock_owner, atime, mtime, ctime;
    uint32_t atimensec, mtimensec, ctimensec, mode, unused4, uid, gid, unused5;
};
struct open_in { uint32_t flags, open_flags; };
struct open_out { uint64_t fh; uint32_t open_flags, padding; };
struct create_in { uint32_t flags, mode, umask, open_flags; };
struct mkdir_in { uint32_t mode, umask; };
struct rename2_in { uint64_t newdir; uint32_t flags, padding; };
struct io_in { uint64_t fh, offset; uint32_t size, io_flags; uint64_t lock_owner; uint32_t flags, padding; };
struct write_out { uint32_t size, padding; };
struct release_in { uint64_t fh; uint32_t flags, release_flags; uint64_t lock_owner; };
struct fsync_in { uint64_t fh; uint32_t fsync_flags, padding; };
struct xattr_in { uint32_t size, flags; };
struct xattr_out { uint32_t size, padding; };
struct lseek_in { uint64_t fh, offset; uint32_t whence, padding; };
struct kstatfs { uint64_t blocks, bfree, bavail, files, ffree; uint32_t bsize, namelen, frsize, padding, spare[6]; };
struct init_in { uint32_t major, minor, max_readahead, flags, flags2, unused[11]; };
struct init_out {
    uint32_t major, minor, max_readahead, flags;
    uint16_t max_background, congestion_threshold;
    uint32_t max_write, time_gran;
    uint16_t max_pages, map_alignment;
    uint32_t flags2, unused[7];
};
struct dirent_l { uint64_t ino, off; uint32_t namelen, type; };

_Static_assert(sizeof(struct in_hdr) == 40, "fuse_in_header");
_Static_assert(sizeof(struct out_hdr) == 16, "fuse_out_header");
_Static_assert(sizeof(struct fattr) == 88, "fuse_attr");
_Static_assert(sizeof(struct entry_out) == 128, "fuse_entry_out (the driver's Lookup takes 0x80)");
_Static_assert(sizeof(struct attr_out) == 104, "fuse_attr_out (GetAttr takes 0x68)");
_Static_assert(sizeof(struct setattr_in) == 88, "fuse_setattr_in (SetAttr sends 0x58)");
_Static_assert(sizeof(struct io_in) == 40, "fuse_read_in / fuse_write_in (0x28)");
_Static_assert(sizeof(struct release_in) == 24, "fuse_release_in (0x18)");
_Static_assert(sizeof(struct kstatfs) == 80, "fuse_kstatfs (StatFS takes 0x50)");
_Static_assert(sizeof(struct init_in) == 64 && sizeof(struct init_out) == 64, "fuse_init_in/out (0x40 each)");

/* ---- Nodes: what each node ID the Mac holds stands for. A node is a name in
   its parent, so a rename moves a whole folder's worth of nodes at once. */

/* struct HvFuseNode and struct HvFuseHandle: virtio_fs_internal.h */

static uint32_t name_hash(uint64_t parent, const char *name) {
    uint32_t h = 2166136261u ^ (uint32_t)(parent * 0x9e3779b9u);
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) h = (h ^ *p) * 16777619u;
    return h;
}

static HvFuseNode *node_get(HvFuse *f, uint64_t id) {
    for (HvFuseNode *n = f->by_id[id % f->nbuckets]; n; n = n->next_id)
        if (n->id == id) return n;
    return NULL;
}

static HvFuseNode *node_find(HvFuse *f, uint64_t parent, const char *name) {
    for (HvFuseNode *n = f->by_name[name_hash(parent, name) % f->nbuckets]; n; n = n->next_name)
        if (n->parent == parent && !strcmp(n->name, name)) return n;
    return NULL;
}

static void name_link(HvFuse *f, HvFuseNode *n) {
    HvFuseNode **b = &f->by_name[name_hash(n->parent, n->name) % f->nbuckets];
    n->next_name = *b;
    *b = n;
    n->linked = 1;
}

static void name_unlink(HvFuse *f, HvFuseNode *n) {
    if (!n->linked) return;
    for (HvFuseNode **p = &f->by_name[name_hash(n->parent, n->name) % f->nbuckets]; *p; p = &(*p)->next_name)
        if (*p == n) {
            *p = n->next_name;
            break;
        }
    n->linked = 0;
}

/* Frees n once nothing refers to it, then its parent if that was all that kept it. */
static void node_release(HvFuse *f, HvFuseNode *n) {
    while (n && !n->pinned && n->nlookup == 0 && n->children == 0) {
        name_unlink(f, n);
        for (HvFuseNode **p = &f->by_id[n->id % f->nbuckets]; *p; p = &(*p)->next_id)
            if (*p == n) {
                *p = n->next_id;
                break;
            }
        HvFuseNode *parent = node_get(f, n->parent);
        free(n->name);
        free(n);
        f->nnodes--;
        if (!parent) break;
        parent->children--;
        n = parent;
    }
}

static HvFuseNode *node_new(HvFuse *f, uint64_t parent, const char *name, int export) {
    HvFuseNode *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->name = strdup(name);
    if (!n->name) {
        free(n);
        return NULL;
    }
    n->id = f->next_id++;
    n->parent = parent;
    n->export = export;
    HvFuseNode **b = &f->by_id[n->id % f->nbuckets];
    n->next_id = *b;
    *b = n;
    name_link(f, n);
    HvFuseNode *p = node_get(f, parent);
    if (p) p->children++;
    f->nnodes++;
    return n;
}

/* The node for parent/name, counted as one more lookup. */
static HvFuseNode *node_child(HvFuse *f, HvFuseNode *parent, const char *name) {
    HvFuseNode *n = node_find(f, parent->id, name);
    if (!n) n = node_new(f, parent->id, name, parent->export);
    if (n) n->nlookup++;
    return n;
}

/* Moves n to parent/name (a rename). */
static void node_move(HvFuse *f, HvFuseNode *n, HvFuseNode *parent, const char *name) {
    char *copy = strdup(name);
    if (!copy) return;
    name_unlink(f, n);
    HvFuseNode *old = node_get(f, n->parent);
    parent->children++;
    n->parent = parent->id;
    n->export = parent->export;
    free(n->name);
    n->name = copy;
    name_link(f, n);
    if (old) {
        old->children--;
        node_release(f, old);
    }
}

/* The node's host path. Returns 0, or a Linux error: the top folder has no
   path, and a removed file is no longer anywhere. */
static int node_path(HvFuse *f, const HvFuseNode *n, char *buf, size_t cap) {
    if (n->id == ROOT_ID) return L_EPERM;
    if (!n->linked) return L_ENOENT;
    if (n->parent == ROOT_ID) {
        if (strlcpy(buf, f->exports[n->export].path, cap) >= cap) return L_ENAMETOOLONG;
        return 0;
    }
    const HvFuseNode *parent = node_get(f, n->parent);
    if (!parent) return L_ENOENT;
    int e = node_path(f, parent, buf, cap);
    if (e) return e;
    size_t len = strlen(buf);
    if (len + 1 + strlen(n->name) + 1 > cap) return L_ENAMETOOLONG;
    buf[len] = '/';
    strcpy(buf + len + 1, n->name);
    return 0;
}

static int child_path(HvFuse *f, const HvFuseNode *parent, const char *name, char *buf, size_t cap) {
    int e = node_path(f, parent, buf, cap);
    if (e) return e;
    size_t len = strlen(buf);
    if (len + 1 + strlen(name) + 1 > cap) return L_ENAMETOOLONG;
    buf[len] = '/';
    strcpy(buf + len + 1, name);
    return 0;
}

static int readonly(const HvFuse *f, const HvFuseNode *n) {
    return n->export < 0 || f->exports[n->export].readonly;
}

/* ---- The host's files. Every path is the export's real path plus names the
   Mac gave, so no symbolic link may appear anywhere along it: one that the
   iPad or the Mac made would otherwise lead out of the shared folder. A link
   as the last name is itself what's asked about (lstat, not stat). */

#define NOLINKS (AT_SYMLINK_NOFOLLOW | AT_SYMLINK_NOFOLLOW_ANY)

static int host_lstat(const char *path, struct stat *st) { return fstatat(AT_FDCWD, path, st, NOLINKS); }

/* The folder the node is, opened, for the calls that only take a name relative to one. */
static int open_folder(HvFuse *f, const HvFuseNode *n, int *fd) {
    char path[PATH_MAX];
    int e = node_path(f, n, path, sizeof path);
    if (e) return e;
    *fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
    return *fd < 0 ? -1 : 0; /* -1: errno says why */
}

/* ---- Answers. */

static int to_linux(int e) {
    switch (e) {
    case 0: return 0;
    case EAGAIN: return L_EAGAIN;
    case ENAMETOOLONG: return L_ENAMETOOLONG;
    case ENOSYS: return L_ENOSYS;
    case ENOTEMPTY: return L_ENOTEMPTY;
    case ELOOP: return L_ELOOP;
    case ENOATTR: return L_ENODATA;
    case ENOTSUP: return L_EOPNOTSUPP;
    case ETIMEDOUT: return L_ETIMEDOUT;
    case EDQUOT: return L_EDQUOT;
    default: return e > 0 && e <= 34 ? e : L_EIO; /* 1 to 34 are the same numbers on both */
    }
}

static uint32_t reply(const struct in_hdr *h, uint8_t *out, int error, uint32_t len) {
    struct out_hdr o = {.len = (uint32_t)sizeof o + (error ? 0 : len), .error = -error, .unique = h->unique};
    memcpy(out, &o, sizeof o);
    return o.len;
}

static void fill_attr(const HvFuse *f, const HvFuseNode *n, const struct stat *st, const struct in_hdr *h, struct fattr *a) {
    memset(a, 0, sizeof *a);
    a->ino = st->st_ino;
    a->size = (uint64_t)st->st_size;
    a->blocks = (uint64_t)st->st_blocks;
    a->atime = (uint64_t)st->st_atimespec.tv_sec;
    a->atimensec = (uint32_t)st->st_atimespec.tv_nsec;
    a->mtime = (uint64_t)st->st_mtimespec.tv_sec;
    a->mtimensec = (uint32_t)st->st_mtimespec.tv_nsec;
    a->ctime = (uint64_t)st->st_ctimespec.tv_sec;
    a->ctimensec = (uint32_t)st->st_ctimespec.tv_nsec;
    a->mode = st->st_mode;
    if (readonly(f, n)) a->mode &= ~0222u;
    a->nlink = st->st_nlink;
    /* The iPad's owner means nothing to the Mac: everything belongs to whoever asks. */
    a->uid = h->uid;
    a->gid = h->gid;
    a->blksize = 4096;
}

static void root_attr(const HvFuse *f, const struct in_hdr *h, struct fattr *a) {
    memset(a, 0, sizeof *a);
    a->ino = ROOT_ID;
    a->atime = a->mtime = a->ctime = (uint64_t)f->born;
    a->mode = S_IFDIR | 0555;
    a->nlink = 2 + (uint32_t)f->nexports;
    a->uid = h->uid;
    a->gid = h->gid;
    a->blksize = 4096;
}

static uint32_t reply_entry(HvFuse *f, const struct in_hdr *h, uint8_t *out, HvFuseNode *n, const struct stat *st) {
    struct entry_out e = {.nodeid = n->id, .entry_valid = VALID_S, .attr_valid = VALID_S};
    fill_attr(f, n, st, h, &e.attr);
    memcpy(out + sizeof(struct out_hdr), &e, sizeof e);
    return reply(h, out, 0, sizeof e);
}

/* A name from the request: one of its own, inside the folder (no "/", "." or ".."). */
static const char *take_name(const uint8_t *arg, uint32_t alen, uint32_t *pos) {
    if (*pos >= alen) return NULL;
    const char *s = (const char *)arg + *pos;
    size_t n = strnlen(s, alen - *pos);
    if (n == alen - *pos) return NULL; /* no terminator */
    *pos += (uint32_t)n + 1;
    return s;
}

static int bad_name(const char *s) {
    return !s || !*s || !strcmp(s, ".") || !strcmp(s, "..") || strchr(s, '/') || strlen(s) > 255;
}

static HvFuseHandle *handle_get(HvFuse *f, uint64_t fh) {
    if (fh == 0 || fh > f->nhandles || !f->handles[fh - 1].used) return NULL;
    return &f->handles[fh - 1];
}

static uint64_t handle_new(HvFuse *f) {
    for (uint32_t i = 0; i < f->nhandles; i++)
        if (!f->handles[i].used) {
            memset(&f->handles[i], 0, sizeof f->handles[i]);
            f->handles[i].used = 1;
            f->handles[i].fd = -1;
            return i + 1;
        }
    uint32_t n = f->nhandles ? f->nhandles * 2 : 64;
    HvFuseHandle *grown = realloc(f->handles, n * sizeof *grown);
    if (!grown) return 0;
    memset(grown + f->nhandles, 0, (n - f->nhandles) * sizeof *grown);
    f->handles = grown;
    uint32_t i = f->nhandles;
    f->nhandles = n;
    f->handles[i].used = 1;
    f->handles[i].fd = -1;
    return i + 1;
}

static void listing_free(HvFuseHandle *hd) {
    for (uint32_t i = 0; i < hd->count; i++) free(hd->names[i]);
    free(hd->names);
    free(hd->types);
    free(hd->inos);
    hd->names = NULL, hd->types = NULL, hd->inos = NULL, hd->count = 0;
}

static void handle_close(HvFuse *f, uint64_t fh) {
    HvFuseHandle *hd = handle_get(f, fh);
    if (!hd) return;
    if (hd->fd >= 0) close(hd->fd);
    listing_free(hd);
    hd->used = 0;
}

static int listing_add(HvFuseHandle *hd, uint32_t *cap, const char *name, uint8_t type, uint64_t ino) {
    if (hd->count == *cap) {
        uint32_t n = *cap ? *cap * 2 : 32;
        char **names = realloc(hd->names, n * sizeof *names);
        if (names) hd->names = names;
        uint8_t *types = realloc(hd->types, n);
        if (types) hd->types = types;
        uint64_t *inos = realloc(hd->inos, n * sizeof *inos);
        if (inos) hd->inos = inos;
        if (!names || !types || !inos) return -1;
        *cap = n;
    }
    char *copy = strdup(name);
    if (!copy) return -1;
    hd->names[hd->count] = copy;
    hd->types[hd->count] = type;
    hd->inos[hd->count] = ino;
    hd->count++;
    return 0;
}

/* What the folder holds now, read when the Mac starts reading it (offset 0). */
static int list_folder(HvFuse *f, HvFuseHandle *hd, HvFuseNode *n) {
    listing_free(hd);
    uint32_t cap = 0;
    if (n->id == ROOT_ID) {
        if (listing_add(hd, &cap, ".", DT_DIR, ROOT_ID) || listing_add(hd, &cap, "..", DT_DIR, ROOT_ID)) return L_ENOMEM;
        for (int i = 0; i < f->nexports; i++) {
            struct stat st;
            uint64_t ino = host_lstat(f->exports[i].path, &st) == 0 ? st.st_ino : 0;
            if (listing_add(hd, &cap, f->exports[i].name, DT_DIR, ino)) return L_ENOMEM;
        }
        return 0;
    }
    char path[PATH_MAX];
    int e = node_path(f, n, path, sizeof path);
    if (e) return e;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
    if (fd < 0) return to_linux(errno);
    DIR *dir = fdopendir(fd);
    if (!dir) {
        e = to_linux(errno);
        close(fd);
        return e;
    }
    struct dirent *d;
    while ((d = readdir(dir)))
        if (listing_add(hd, &cap, d->d_name, d->d_type, d->d_ino)) {
            closedir(dir);
            return L_ENOMEM;
        }
    closedir(dir);
    return 0;
}

/* Linux open flags to the host's. Write-only opens read too where allowed:
   the Mac's page cache fills a partly written page through the same handle. */
static int open_flags(uint32_t l, int *upgraded) {
    int acc = (int)(l & L_O_ACCMODE);
    *upgraded = acc == L_O_WRONLY;
    int fl = (*upgraded ? O_RDWR : acc) | O_CLOEXEC | O_NOFOLLOW_ANY;
    if (l & L_O_TRUNC) fl |= O_TRUNC;
    if (l & L_O_CREAT) fl |= O_CREAT;
    if (l & L_O_EXCL) fl |= O_EXCL;
    /* O_APPEND is left out: the Mac sends every write's offset itself. */
    return fl;
}

static int open_host(const char *path, uint32_t lflags, mode_t mode) {
    int upgraded;
    int fl = open_flags(lflags, &upgraded);
    int fd = open(path, fl, mode);
    if (fd < 0 && upgraded && errno == EACCES) fd = open(path, (fl & ~O_RDWR) | O_WRONLY, mode);
    return fd;
}

static int writes(uint32_t lflags) {
    return (lflags & L_O_ACCMODE) != 0 || (lflags & (L_O_TRUNC | L_O_CREAT));
}

static const char *op_name(uint32_t op) {
    static const char *names[] = {
        [OP_LOOKUP] = "LOOKUP", [OP_FORGET] = "FORGET", [OP_GETATTR] = "GETATTR", [OP_SETATTR] = "SETATTR",
        [OP_READLINK] = "READLINK", [OP_SYMLINK] = "SYMLINK", [OP_MKNOD] = "MKNOD", [OP_MKDIR] = "MKDIR",
        [OP_UNLINK] = "UNLINK", [OP_RMDIR] = "RMDIR", [OP_RENAME] = "RENAME", [OP_LINK] = "LINK", [OP_OPEN] = "OPEN",
        [OP_READ] = "READ", [OP_WRITE] = "WRITE", [OP_STATFS] = "STATFS", [OP_RELEASE] = "RELEASE",
        [OP_FSYNC] = "FSYNC", [OP_SETXATTR] = "SETXATTR", [OP_GETXATTR] = "GETXATTR", [OP_LISTXATTR] = "LISTXATTR",
        [OP_REMOVEXATTR] = "REMOVEXATTR", [OP_FLUSH] = "FLUSH", [OP_INIT] = "INIT", [OP_OPENDIR] = "OPENDIR",
        [OP_READDIR] = "READDIR", [OP_RELEASEDIR] = "RELEASEDIR", [OP_FSYNCDIR] = "FSYNCDIR", [OP_ACCESS] = "ACCESS",
        [OP_CREATE] = "CREATE", [OP_INTERRUPT] = "INTERRUPT", [OP_DESTROY] = "DESTROY",
        [OP_BATCH_FORGET] = "BATCH_FORGET", [OP_RENAME2] = "RENAME2", [OP_LSEEK] = "LSEEK",
    };
    return op < sizeof names / sizeof *names && names[op] ? names[op] : "?";
}

void hv_fuse_init(HvFuse *f) {
    memset(f, 0, sizeof *f);
    f->nbuckets = 4096;
    f->by_id = calloc(f->nbuckets, sizeof *f->by_id);
    f->by_name = calloc(f->nbuckets, sizeof *f->by_name);
    f->next_id = ROOT_ID;
    f->max_write = 128 * 1024;
    f->born = time(NULL);
    f->log = getenv("HV_FS_LOG") != NULL;
    HvFuseNode *root = calloc(1, sizeof *root);
    root->id = f->next_id++;
    root->name = strdup("");
    root->export = -1;
    root->pinned = 1;
    f->by_id[root->id % f->nbuckets] = root;
    f->nnodes = 1;
}

int hv_fuse_export(HvFuse *f, const char *name, const char *host_dir, int ro) {
    struct stat st;
    if (f->nexports == HV_FUSE_MAX_EXPORTS || bad_name(name) || strlen(name) >= sizeof f->exports[0].name) return -1;
    if (!host_dir || stat(host_dir, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    int i = f->nexports;
    /* Its real path: /var and /tmp are themselves links, on the iPad as here. */
    char *path = realpath(host_dir, NULL);
    if (!path) return -1;
    strlcpy(f->exports[i].name, name, sizeof f->exports[i].name);
    f->exports[i].path = path;
    f->exports[i].readonly = ro;
    f->nexports++;
    HvFuseNode *n = node_new(f, ROOT_ID, name, i);
    if (!n) return -1;
    n->pinned = 1;
    return 0;
}

void hv_fuse_free(HvFuse *f) {
    for (uint32_t i = 0; i < f->nhandles; i++) handle_close(f, i + 1);
    free(f->handles);
    for (uint32_t b = 0; f->by_id && b < f->nbuckets; b++)
        for (HvFuseNode *n = f->by_id[b], *next; n; n = next) {
            next = n->next_id;
            free(n->name);
            free(n);
        }
    free(f->by_id);
    free(f->by_name);
    for (int i = 0; i < f->nexports; i++) free(f->exports[i].path);
    memset(f, 0, sizeof *f);
}

/* ---- Suspend (virtio_fs_internal.h): the records put back as they were saved. */

void hv_fuse_clear(HvFuse *f) {
    for (uint32_t i = 0; i < f->nhandles; i++) handle_close(f, i + 1);
    for (uint32_t b = 0; b < f->nbuckets; b++) {
        for (HvFuseNode *n = f->by_id[b], *next; n; n = next) {
            next = n->next_id;
            free(n->name);
            free(n);
        }
        f->by_id[b] = f->by_name[b] = NULL;
    }
    f->nnodes = 0;
}

HvFuseNode *hv_fuse_node(HvFuse *f, uint64_t id) { return node_get(f, id); }

int hv_fuse_add_node(HvFuse *f, uint64_t id, uint64_t parent, const char *name, int export, uint64_t nlookup, int linked,
                     int pinned) {
    if (node_get(f, id)) return -1;
    HvFuseNode *n = calloc(1, sizeof *n);
    if (!n || !(n->name = strdup(name))) return free(n), -1;
    n->id = id, n->parent = parent, n->export = export, n->nlookup = nlookup, n->pinned = pinned;
    HvFuseNode **b = &f->by_id[id % f->nbuckets];
    n->next_id = *b;
    *b = n;
    if (linked) name_link(f, n);
    f->nnodes++;
    return 0;
}

void hv_fuse_count_children(HvFuse *f) {
    for (uint32_t b = 0; b < f->nbuckets; b++)
        for (HvFuseNode *n = f->by_id[b]; n; n = n->next_id) n->children = 0;
    for (uint32_t b = 0; b < f->nbuckets; b++)
        for (HvFuseNode *n = f->by_id[b]; n; n = n->next_id) {
            HvFuseNode *p = n->id == HV_FUSE_ROOT_ID ? NULL : node_get(f, n->parent);
            if (p) p->children++;
        }
}

HvFuseHandle *hv_fuse_add_handle(HvFuse *f, uint64_t fh) {
    if (fh == 0 || fh > (1u << 20)) return NULL;
    while (f->nhandles < fh) {
        uint32_t n = f->nhandles ? f->nhandles * 2 : 64;
        HvFuseHandle *grown = realloc(f->handles, n * sizeof *grown);
        if (!grown) return NULL;
        memset(grown + f->nhandles, 0, (n - f->nhandles) * sizeof *grown);
        f->handles = grown;
        f->nhandles = n;
    }
    HvFuseHandle *hd = &f->handles[fh - 1];
    if (hd->used) return NULL;
    memset(hd, 0, sizeof *hd);
    hd->used = 1;
    hd->fd = -1;
    return hd;
}

int hv_fuse_listing_add(HvFuseHandle *hd, uint32_t *cap, const char *name, uint8_t type, uint64_t ino) {
    return listing_add(hd, cap, name, type, ino);
}

int hv_fuse_reopen(HvFuse *f, HvFuseHandle *hd, int access) {
    HvFuseNode *n = node_get(f, hd->node);
    if (!n) return L_ENOENT;
    char path[PATH_MAX];
    int e = node_path(f, n, path, sizeof path);
    if (e) return e;
    int fd = open(path, (access == 2 ? O_RDWR : access == 1 ? O_WRONLY : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW_ANY);
    if (fd < 0) return to_linux(errno);
    struct stat st;
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        close(fd);
        return L_EISDIR;
    }
    hd->fd = fd;
    return 0;
}

int hv_fuse_name_ok(const char *name) { return !bad_name(name); }

/* ---- Requests. Each returns the answer's length (from reply()). */

/* A new connection (the Mac restarted its driver, or mounted again): nothing it
   was told before still holds. Only the top folder and the exports remain. */
static void fuse_forget_all(HvFuse *f) {
    for (uint32_t i = 0; i < f->nhandles; i++) handle_close(f, i + 1);
    for (uint32_t b = 0; b < f->nbuckets; b++)
        for (HvFuseNode **p = &f->by_id[b]; *p;) {
            HvFuseNode *n = *p;
            if (n->pinned) {
                n->nlookup = 0;
                n->children = n->id == ROOT_ID ? (uint32_t)f->nexports : 0;
                p = &n->next_id;
                continue;
            }
            name_unlink(f, n);
            *p = n->next_id;
            free(n->name);
            free(n);
            f->nnodes--;
        }
}

static uint32_t do_init(HvFuse *f, const struct in_hdr *h, const uint8_t *arg, uint32_t alen, uint8_t *out) {
    struct init_in in = {0};
    memcpy(&in, arg, alen < sizeof in ? alen : sizeof in);
    if (in.major < 7) return reply(h, out, L_EPROTO, 0);
    fuse_forget_all(f);
    struct init_out o = {0};
    o.major = 7;
    o.minor = 31;
    o.max_readahead = in.max_readahead;
    o.flags = in.flags & (FUSE_ASYNC_READ | FUSE_ATOMIC_O_TRUNC | FUSE_BIG_WRITES | FUSE_MAX_PAGES);
    o.max_background = 16;
    o.congestion_threshold = 12;
    o.max_write = f->max_write;
    o.time_gran = 1;
    o.max_pages = (uint16_t)(f->max_write / 4096);
    fprintf(stderr, "virtio-fs: the Mac connected (FUSE %u.%u, flags %#x), %d folder%s shared\n", in.major, in.minor, in.flags,
            f->nexports, f->nexports == 1 ? "" : "s");
    memcpy(out + sizeof(struct out_hdr), &o, sizeof o);
    return reply(h, out, 0, sizeof o);
}

static uint32_t empty(const struct in_hdr *h, uint8_t *out, int error) { return reply(h, out, error, 0); }

static uint32_t entry_for(HvFuse *f, const struct in_hdr *h, uint8_t *out, HvFuseNode *parent, const char *name, const char *path) {
    struct stat st;
    if (host_lstat(path, &st) != 0) return empty(h, out, to_linux(errno));
    HvFuseNode *c = node_child(f, parent, name);
    if (!c) return empty(h, out, L_ENOMEM);
    return reply_entry(f, h, out, c, &st);
}

static uint32_t open_reply(HvFuse *f, const struct in_hdr *h, uint8_t *out, uint32_t at, HvFuseNode *n, int fd, int dir) {
    uint64_t fh = handle_new(f);
    if (!fh) {
        if (fd >= 0) close(fd);
        return empty(h, out, L_ENOMEM);
    }
    HvFuseHandle *hd = &f->handles[fh - 1];
    hd->fd = fd;
    hd->dir = dir;
    hd->node = n->id;
    hd->readonly = readonly(f, n);
    struct open_out o = {.fh = fh};
    memcpy(out + sizeof(struct out_hdr) + at, &o, sizeof o);
    return reply(h, out, 0, at + (uint32_t)sizeof o);
}

static void forget(HvFuse *f, uint64_t id, uint64_t k) {
    HvFuseNode *n = node_get(f, id);
    if (!n) return;
    n->nlookup -= k < n->nlookup ? k : n->nlookup;
    node_release(f, n);
}

#define ARG(var)                                                         \
    if (alen < sizeof var) return empty(h, out, L_EINVAL);              \
    memcpy(&var, arg, sizeof var)

static uint32_t dispatch(HvFuse *f, const struct in_hdr *h, const uint8_t *arg, uint32_t alen, uint8_t *out, uint32_t room) {
    uint8_t *body = out + sizeof(struct out_hdr);
    switch (h->opcode) {
    case OP_INIT: return do_init(f, h, arg, alen, out);
    case OP_DESTROY:
        for (uint32_t i = 0; i < f->nhandles; i++) handle_close(f, i + 1);
        return empty(h, out, 0);
    case OP_INTERRUPT: return 0; /* every request finishes promptly anyway */
    case OP_RELEASE:
    case OP_RELEASEDIR: { /* by handle alone: its node may be gone by now */
        struct release_in r = {0};
        memcpy(&r, arg, alen < sizeof r ? alen : sizeof r);
        handle_close(f, r.fh);
        return empty(h, out, 0);
    }
    case OP_BATCH_FORGET: {
        uint32_t count = 0;
        if (alen >= 8) memcpy(&count, arg, 4);
        for (uint32_t i = 0; i < count && 8 + (i + 1) * 16 <= alen; i++) {
            uint64_t one[2];
            memcpy(one, arg + 8 + i * 16, 16);
            forget(f, one[0], one[1]);
        }
        return 0;
    }
    case OP_FORGET: {
        uint64_t k = 0;
        if (alen >= 8) memcpy(&k, arg, 8);
        forget(f, h->nodeid, k);
        return 0;
    }
    default: break;
    }

    HvFuseNode *n = node_get(f, h->nodeid);
    if (!n) return empty(h, out, L_ENOENT);
    char path[PATH_MAX], path2[PATH_MAX];
    struct stat st;
    uint32_t pos = 0;
    int e;

    switch (h->opcode) {
    case OP_LOOKUP: {
        const char *name = take_name(arg, alen, &pos);
        if (bad_name(name)) return empty(h, out, name ? L_ENOENT : L_EINVAL);
        if (n->id == ROOT_ID) {
            HvFuseNode *c = node_find(f, ROOT_ID, name);
            if (!c || c->export < 0 || host_lstat(f->exports[c->export].path, &st) != 0) return empty(h, out, L_ENOENT);
            c->nlookup++;
            return reply_entry(f, h, out, c, &st);
        }
        if ((e = child_path(f, n, name, path, sizeof path))) return empty(h, out, e);
        return entry_for(f, h, out, n, name, path);
    }

    case OP_GETATTR: {
        struct getattr_in g = {0};
        memcpy(&g, arg, alen < sizeof g ? alen : sizeof g);
        struct attr_out a = {.attr_valid = VALID_S};
        if (n->id == ROOT_ID) {
            root_attr(f, h, &a.attr);
        } else {
            HvFuseHandle *hd = (g.flags & FUSE_GETATTR_FH) ? handle_get(f, g.fh) : NULL;
            if (hd && hd->fd >= 0) {
                if (fstat(hd->fd, &st) != 0) return empty(h, out, to_linux(errno));
            } else {
                if ((e = node_path(f, n, path, sizeof path))) return empty(h, out, e);
                if (host_lstat(path, &st) != 0) return empty(h, out, to_linux(errno));
            }
            fill_attr(f, n, &st, h, &a.attr);
        }
        memcpy(body, &a, sizeof a);
        return reply(h, out, 0, sizeof a);
    }

    case OP_SETATTR: {
        struct setattr_in s;
        ARG(s);
        if (readonly(f, n)) return empty(h, out, L_EROFS);
        HvFuseHandle *hd = (s.valid & FATTR_FH) ? handle_get(f, s.fh) : NULL;
        int fd = hd && hd->fd >= 0 ? hd->fd : -1;
        e = node_path(f, n, path, sizeof path);
        if (e && fd < 0) return empty(h, out, e);
        if (s.valid & FATTR_MODE) {
            int r = fd >= 0 ? fchmod(fd, (mode_t)(s.mode & 07777)) : fchmodat(AT_FDCWD, path, (mode_t)(s.mode & 07777), NOLINKS);
            if (r != 0) return empty(h, out, to_linux(errno));
        }
        if (s.valid & FATTR_SIZE) {
            int r = fd >= 0 ? ftruncate(fd, (off_t)s.size) : -1, err = errno;
            if (r != 0 && (fd < 0 || err == EBADF || err == EINVAL) && !e) { /* by name; a read-only handle can't */
                int t = open(path, O_WRONLY | O_NOFOLLOW_ANY | O_CLOEXEC);
                if (t < 0) return empty(h, out, to_linux(errno));
                r = ftruncate(t, (off_t)s.size), err = errno;
                close(t);
            }
            if (r != 0) return empty(h, out, to_linux(err));
        }
        if (s.valid & (FATTR_ATIME | FATTR_MTIME)) {
            struct timespec ts[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
            if (s.valid & FATTR_ATIME)
                ts[0] = (s.valid & FATTR_ATIME_NOW) ? (struct timespec){0, UTIME_NOW} : (struct timespec){(time_t)s.atime, s.atimensec};
            if (s.valid & FATTR_MTIME)
                ts[1] = (s.valid & FATTR_MTIME_NOW) ? (struct timespec){0, UTIME_NOW} : (struct timespec){(time_t)s.mtime, s.mtimensec};
            int r = fd >= 0 ? futimens(fd, ts) : utimensat(AT_FDCWD, path, ts, NOLINKS);
            if (r != 0) return empty(h, out, to_linux(errno));
        }
        /* An owner change is let pass: files here belong to whoever looks (fill_attr). */
        if (fd >= 0 ? fstat(fd, &st) : host_lstat(path, &st)) return empty(h, out, to_linux(errno));
        struct attr_out a = {.attr_valid = VALID_S};
        fill_attr(f, n, &st, h, &a.attr);
        memcpy(body, &a, sizeof a);
        return reply(h, out, 0, sizeof a);
    }

    case OP_READLINK: {
        HvFuseNode *parent = node_get(f, n->parent);
        int dir;
        if (!parent || n->parent == ROOT_ID || !n->linked) return empty(h, out, n->linked ? L_EINVAL : L_ENOENT);
        if ((e = open_folder(f, parent, &dir))) return empty(h, out, e < 0 ? to_linux(errno) : e);
        ssize_t k = readlinkat(dir, n->name, (char *)body, room);
        int err = errno;
        close(dir);
        if (k < 0) return empty(h, out, to_linux(err));
        return reply(h, out, 0, (uint32_t)k);
    }

    case OP_SYMLINK: {
        const char *name = take_name(arg, alen, &pos), *target = take_name(arg, alen, &pos);
        if (bad_name(name) || !target || !*target) return empty(h, out, L_EINVAL);
        if (readonly(f, n)) return empty(h, out, L_EROFS);
        if ((e = child_path(f, n, name, path, sizeof path))) return empty(h, out, e);
        int dir;
        if ((e = open_folder(f, n, &dir))) return empty(h, out, e < 0 ? to_linux(errno) : e);
        int r = symlinkat(target, dir, name), err = errno;
        close(dir);
        if (r != 0) return empty(h, out, to_linux(err));
        return entry_for(f, h, out, n, name, path);
    }

    case OP_MKNOD: return empty(h, out, L_EPERM); /* devices and pipes have no place on the iPad */

    case OP_MKDIR: {
        struct mkdir_in m;
        ARG(m);
        pos = sizeof m;
        const char *name = take_name(arg, alen, &pos);
        if (bad_name(name)) return empty(h, out, L_EINVAL);
        if (readonly(f, n)) return empty(h, out, L_EROFS);
        if ((e = child_path(f, n, name, path, sizeof path))) return empty(h, out, e);
        int dir;
        if ((e = open_folder(f, n, &dir))) return empty(h, out, e < 0 ? to_linux(errno) : e);
        int r = mkdirat(dir, name, (mode_t)(m.mode & ~m.umask & 07777)), err = errno;
        close(dir);
        if (r != 0) return empty(h, out, to_linux(err));
        return entry_for(f, h, out, n, name, path);
    }

    case OP_UNLINK:
    case OP_RMDIR: {
        const char *name = take_name(arg, alen, &pos);
        if (bad_name(name)) return empty(h, out, L_EINVAL);
        if (readonly(f, n)) return empty(h, out, L_EROFS);
        if ((e = child_path(f, n, name, path, sizeof path))) return empty(h, out, e);
        if (unlinkat(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW_ANY | (h->opcode == OP_RMDIR ? AT_REMOVEDIR : 0)) != 0) return empty(h, out, to_linux(errno));
        HvFuseNode *c = node_find(f, n->id, name);
        if (c) {
            name_unlink(f, c); /* still open, perhaps, but no longer anywhere */
            node_release(f, c);
        }
        return empty(h, out, 0);
    }

    case OP_RENAME:
    case OP_RENAME2: {
        struct rename2_in r = {0};
        uint32_t fixed = h->opcode == OP_RENAME ? 8 : 16;
        if (alen < fixed) return empty(h, out, L_EINVAL);
        memcpy(&r, arg, fixed);
        pos = fixed;
        const char *from = take_name(arg, alen, &pos), *to = take_name(arg, alen, &pos);
        if (bad_name(from) || bad_name(to)) return empty(h, out, L_EINVAL);
        HvFuseNode *np = node_get(f, r.newdir);
        if (!np) return empty(h, out, L_ENOENT);
        if (readonly(f, n) || readonly(f, np)) return empty(h, out, L_EROFS);
        if ((e = child_path(f, n, from, path, sizeof path)) || (e = child_path(f, np, to, path2, sizeof path2))) return empty(h, out, e);
        unsigned how = RENAME_NOFOLLOW_ANY | (r.flags & RENAME_EXCHANGE_L ? RENAME_SWAP : 0) | (r.flags & RENAME_NOREPLACE_L ? RENAME_EXCL : 0);
        int ok = renameatx_np(AT_FDCWD, path, AT_FDCWD, path2, how);
        if (ok != 0) return empty(h, out, to_linux(errno));
        HvFuseNode *moved = node_find(f, n->id, from), *dest = node_find(f, np->id, to);
        if (moved == dest) return empty(h, out, 0); /* the same name: nothing moved */
        /* Both folders stay while names move between them; let go of them by ID
           afterwards, since releasing one may release the other. */
        uint64_t a = n->id, b = np->id;
        n->nlookup++, np->nlookup++;
        if (r.flags & RENAME_EXCHANGE_L) {
            if (moved) name_unlink(f, moved);
            if (dest) name_unlink(f, dest);
            if (moved) node_move(f, moved, np, to);
            if (dest) node_move(f, dest, n, from);
        } else {
            if (dest) {
                name_unlink(f, dest); /* replaced */
                node_release(f, dest);
            }
            if (moved) node_move(f, moved, np, to);
        }
        n->nlookup--, np->nlookup--; /* n == np: both counts were on the one node */
        if ((n = node_get(f, a))) node_release(f, n);
        if ((np = node_get(f, b))) node_release(f, np);
        return empty(h, out, 0);
    }

    case OP_LINK: {
        uint64_t old_id;
        ARG(old_id);
        pos = sizeof old_id;
        const char *name = take_name(arg, alen, &pos);
        HvFuseNode *old = node_get(f, old_id);
        if (bad_name(name)) return empty(h, out, L_EINVAL);
        if (!old) return empty(h, out, L_ENOENT);
        if (readonly(f, n)) return empty(h, out, L_EROFS);
        HvFuseNode *oldparent = node_get(f, old->parent);
        if (!oldparent || old->parent == ROOT_ID) return empty(h, out, L_EPERM); /* the shared folders themselves */
        if (!old->linked) return empty(h, out, L_ENOENT);
        if ((e = child_path(f, n, name, path2, sizeof path2))) return empty(h, out, e);
        int from, to;
        if ((e = open_folder(f, oldparent, &from))) return empty(h, out, e < 0 ? to_linux(errno) : e);
        if ((e = open_folder(f, n, &to))) {
            e = e < 0 ? to_linux(errno) : e;
            close(from);
            return empty(h, out, e);
        }
        int r = linkat(from, old->name, to, name, 0), err = errno;
        close(from), close(to);
        if (r != 0) return empty(h, out, to_linux(err));
        return entry_for(f, h, out, n, name, path2);
    }

    case OP_OPEN: {
        struct open_in o;
        ARG(o);
        if (n->id == ROOT_ID) return empty(h, out, L_EISDIR);
        if (readonly(f, n) && writes(o.flags)) return empty(h, out, L_EROFS);
        if ((e = node_path(f, n, path, sizeof path))) return empty(h, out, e);
        int fd = open_host(path, o.flags & ~(uint32_t)(L_O_CREAT | L_O_EXCL), 0);
        if (fd < 0) return empty(h, out, to_linux(errno));
        return open_reply(f, h, out, 0, n, fd, 0);
    }

    case OP_CREATE: {
        struct create_in c;
        ARG(c);
        pos = sizeof c;
        const char *name = take_name(arg, alen, &pos);
        if (bad_name(name)) return empty(h, out, L_EINVAL);
        if (readonly(f, n)) return empty(h, out, L_EROFS);
        if (room < sizeof(struct entry_out) + sizeof(struct open_out)) return empty(h, out, L_EINVAL);
        if ((e = child_path(f, n, name, path, sizeof path))) return empty(h, out, e);
        int fd = open_host(path, c.flags | L_O_CREAT, (mode_t)(c.mode & ~c.umask & 07777));
        if (fd < 0) return empty(h, out, to_linux(errno));
        HvFuseNode *made;
        if (fstat(fd, &st) != 0 || !(made = node_child(f, n, name))) {
            int err = errno;
            close(fd);
            return empty(h, out, to_linux(err ? err : ENOMEM));
        }
        struct entry_out en = {.nodeid = made->id, .entry_valid = VALID_S, .attr_valid = VALID_S};
        fill_attr(f, made, &st, h, &en.attr);
        memcpy(body, &en, sizeof en);
        return open_reply(f, h, out, sizeof en, made, fd, 0);
    }

    case OP_READ: {
        struct io_in r;
        ARG(r);
        HvFuseHandle *hd = handle_get(f, r.fh);
        if (!hd || hd->fd < 0) return empty(h, out, L_EBADF);
        uint32_t size = r.size < room ? r.size : room;
        ssize_t k = pread(hd->fd, body, size, (off_t)r.offset);
        if (k < 0) return empty(h, out, to_linux(errno));
        return reply(h, out, 0, (uint32_t)k);
    }

    case OP_WRITE: {
        struct io_in w;
        ARG(w);
        HvFuseHandle *hd = handle_get(f, w.fh);
        if (!hd || hd->fd < 0) return empty(h, out, L_EBADF);
        if (hd->readonly) return empty(h, out, L_EROFS);
        if (alen - sizeof w < w.size) return empty(h, out, L_EINVAL);
        ssize_t k = pwrite(hd->fd, arg + sizeof w, w.size, (off_t)w.offset);
        if (k < 0) return empty(h, out, to_linux(errno));
        struct write_out o = {.size = (uint32_t)k};
        memcpy(body, &o, sizeof o);
        return reply(h, out, 0, sizeof o);
    }

    case OP_FLUSH: return empty(h, out, 0); /* writes go straight to the file */
    case OP_FSYNCDIR: return empty(h, out, 0);

    case OP_FSYNC: {
        struct fsync_in s = {0};
        memcpy(&s, arg, alen < sizeof s ? alen : sizeof s);
        HvFuseHandle *hd = handle_get(f, s.fh);
        if (hd && hd->fd >= 0 && fsync(hd->fd) != 0) return empty(h, out, to_linux(errno));
        return empty(h, out, 0);
    }

    case OP_OPENDIR: {
        if (n->id != ROOT_ID) {
            if ((e = node_path(f, n, path, sizeof path))) return empty(h, out, e);
            if (host_lstat(path, &st) != 0) return empty(h, out, to_linux(errno));
            if (!S_ISDIR(st.st_mode)) return empty(h, out, L_ENOTDIR);
        }
        return open_reply(f, h, out, 0, n, -1, 1);
    }

    case OP_READDIR: {
        struct io_in r;
        ARG(r);
        HvFuseHandle *hd = handle_get(f, r.fh);
        if (!hd || !hd->dir) return empty(h, out, L_EBADF);
        if (r.offset == 0 || !hd->names)
            if ((e = list_folder(f, hd, n))) return empty(h, out, e);
        uint32_t size = r.size < room ? r.size : room, used = 0;
        for (uint64_t i = r.offset; i < hd->count; i++) {
            uint32_t len = (uint32_t)strlen(hd->names[i]), rec = (uint32_t)(sizeof(struct dirent_l) + len + 7) & ~7u;
            if (used + rec > size) break;
            struct dirent_l d = {.ino = hd->inos[i], .off = i + 1, .namelen = len, .type = hd->types[i]};
            memcpy(body + used, &d, sizeof d);
            memcpy(body + used + sizeof d, hd->names[i], len);
            memset(body + used + sizeof d + len, 0, rec - sizeof d - len);
            used += rec;
        }
        return reply(h, out, 0, used);
    }

    case OP_STATFS: {
        int x = n->export >= 0 ? n->export : 0;
        struct kstatfs k = {.namelen = 255, .bsize = 4096, .frsize = 4096};
        struct statfs s;
        if (f->nexports && statfs(f->exports[x].path, &s) == 0) {
            k.blocks = s.f_blocks, k.bfree = s.f_bfree, k.bavail = s.f_bavail, k.files = s.f_files, k.ffree = s.f_ffree;
            k.bsize = k.frsize = s.f_bsize;
        }
        memcpy(body, &k, sizeof k);
        return reply(h, out, 0, sizeof k);
    }

    case OP_ACCESS: {
        uint32_t mask = 0;
        if (alen >= 4) memcpy(&mask, arg, 4);
        return empty(h, out, (mask & 2) && readonly(f, n) ? L_EROFS : 0);
    }

    case OP_GETXATTR:
    case OP_LISTXATTR: {
        struct xattr_in x;
        ARG(x);
        pos = sizeof x;
        const char *name = NULL;
        if (h->opcode == OP_GETXATTR && !(name = take_name(arg, alen, &pos))) return empty(h, out, L_EINVAL);
        ssize_t k = 0;
        uint32_t size = x.size < room ? x.size : room;
        if (n->id == ROOT_ID) {
            if (name) return empty(h, out, L_ENODATA);
        } else {
            if ((e = node_path(f, n, path, sizeof path))) return empty(h, out, e);
            k = name ? getxattr(path, name, x.size ? body : NULL, size, 0, XATTR_NOFOLLOW | XATTR_NOFOLLOW_ANY)
                     : listxattr(path, x.size ? (char *)body : NULL, size, XATTR_NOFOLLOW | XATTR_NOFOLLOW_ANY);
            if (k < 0) return empty(h, out, to_linux(errno));
        }
        if (x.size) return reply(h, out, 0, (uint32_t)k);
        struct xattr_out o = {.size = (uint32_t)k};
        memcpy(body, &o, sizeof o);
        return reply(h, out, 0, sizeof o);
    }

    case OP_SETXATTR: {
        struct xattr_in x;
        ARG(x);
        pos = sizeof x;
        const char *name = take_name(arg, alen, &pos);
        if (!name || alen - pos < x.size) return empty(h, out, L_EINVAL);
        if (readonly(f, n)) return empty(h, out, L_EROFS);
        if ((e = node_path(f, n, path, sizeof path))) return empty(h, out, e);
        int fl = XATTR_NOFOLLOW | XATTR_NOFOLLOW_ANY | (x.flags & 1 ? XATTR_CREATE : 0) | (x.flags & 2 ? XATTR_REPLACE : 0);
        if (setxattr(path, name, arg + pos, x.size, 0, fl) != 0) return empty(h, out, to_linux(errno));
        return empty(h, out, 0);
    }

    case OP_REMOVEXATTR: {
        const char *name = take_name(arg, alen, &pos);
        if (!name) return empty(h, out, L_EINVAL);
        if (readonly(f, n)) return empty(h, out, L_EROFS);
        if ((e = node_path(f, n, path, sizeof path))) return empty(h, out, e);
        if (removexattr(path, name, XATTR_NOFOLLOW | XATTR_NOFOLLOW_ANY) != 0) return empty(h, out, to_linux(errno));
        return empty(h, out, 0);
    }

    case OP_LSEEK: {
        struct lseek_in l;
        ARG(l);
        HvFuseHandle *hd = handle_get(f, l.fh);
        if (!hd || hd->fd < 0) return empty(h, out, L_EBADF);
        int whence = l.whence == 3 ? SEEK_DATA : l.whence == 4 ? SEEK_HOLE : (int)l.whence; /* Linux numbers them the other way */
        off_t o = lseek(hd->fd, (off_t)l.offset, whence);
        if (o < 0) return empty(h, out, to_linux(errno));
        uint64_t at = (uint64_t)o;
        memcpy(body, &at, sizeof at);
        return reply(h, out, 0, sizeof at);
    }

    default: return empty(h, out, L_ENOSYS);
    }
}

uint32_t hv_fuse_handle(HvFuse *f, const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_cap) {
    struct in_hdr h;
    if (in_len < sizeof h || out_cap < sizeof(struct out_hdr)) return 0;
    memcpy(&h, in, sizeof h);
    if (h.len >= sizeof h && h.len < in_len) in_len = h.len;
    uint32_t n = dispatch(f, &h, in + sizeof h, in_len - (uint32_t)sizeof h, out, out_cap - (uint32_t)sizeof(struct out_hdr));
    f->requests++;
    if (h.opcode < 64) f->seen[h.opcode]++;
    int32_t err = 0;
    if (n >= sizeof(struct out_hdr)) memcpy(&err, out + 4, 4);
    if (err) f->errors++;
    /* The first requests show how the Mac mounts it; after that, only failures
       other than the everyday "no such file" and "no such attribute". */
    static int logged_failures;
    int quiet_error = err == -L_ENOENT || err == -L_ENODATA;
    if (f->log || f->requests <= 60 || (err && !quiet_error && logged_failures++ < 60) || (err == -L_ENOSYS && h.opcode != OP_MKNOD))
        fprintf(stderr, "virtio-fs: %s node %llu -> %s%d (%u bytes)\n", op_name(h.opcode), (unsigned long long)h.nodeid,
                err ? "error " : "ok ", err ? -err : 0, n);
    return n;
}

/* ---- virtio transport. */

static uint32_t readable_len(const HvVirtqElem *e) {
    uint32_t n = 0;
    for (int s = 0; s < e->nseg; s++)
        if (!e->seg[s].write) n += e->seg[s].len;
    return n;
}

static void *worker(void *opaque) {
    HvVirtioFs *d = opaque;
    for (;;) {
        pthread_mutex_lock(&d->mu);
        while (__atomic_load_n(&d->completed, __ATOMIC_RELAXED) == __atomic_load_n(&d->submitted, __ATOMIC_ACQUIRE))
            pthread_cond_wait(&d->cv, &d->mu);
        pthread_mutex_unlock(&d->mu);
        HvVfsReq *r = &d->ring[d->completed % HV_VFS_RING];
        uint32_t in_len = readable_len(&r->e), cap = hv_virtq_writable(&r->e);
        if (cap > HV_VFS_BUF) cap = HV_VFS_BUF;
        r->written = 0;
        if (in_len > HV_VFS_BUF) {
            /* Bigger than any request the Mac was told it may send: refuse it. */
            struct in_hdr h = {0};
            hv_virtq_read(&d->vio, &r->e, 0, &h, sizeof h);
            if (cap >= sizeof(struct out_hdr)) {
                uint32_t n = reply(&h, d->out, L_EINVAL, 0);
                r->written = hv_virtq_write(&d->vio, &r->e, 0, d->out, n);
            }
        } else if (hv_virtq_read(&d->vio, &r->e, 0, d->in, in_len) == in_len) {
            uint32_t n = hv_fuse_handle(&d->fuse, d->in, in_len, d->out, cap);
            if (n) r->written = hv_virtq_write(&d->vio, &r->e, 0, d->out, n);
        }
        __atomic_store_n(&d->completed, d->completed + 1, __ATOMIC_RELEASE);
        hv_machine_wake(d->vio.m);
    }
    return NULL;
}

/* CPU thread: hand what the driver made available to the worker, while there's room.
   macOS puts every request on queue 0 (the spec's high-priority queue); any queue is served. */
static void notify(HvVirtio *v, int q) {
    HvVirtioFs *d = v->dev;
    int any = 0;
    while (d->submitted - d->published < HV_VFS_RING) {
        HvVfsReq *r = &d->ring[d->submitted % HV_VFS_RING];
        int got = hv_virtq_pop(v, q, &r->e);
        if (got == 0) break;
        r->q = q;
        if (got < 0) { /* a chain this model cannot follow: complete it empty, or the slot is lost */
            fprintf(stderr, "virtio-fs: broken request chain (%d segments), failing it\n", r->e.nseg);
            hv_virtq_push(v, q, &r->e, 0);
            hv_virtio_interrupt(v);
            continue;
        }
        pthread_mutex_lock(&d->mu);
        __atomic_store_n(&d->submitted, d->submitted + 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&d->mu);
        any = 1;
    }
    if (any) pthread_cond_signal(&d->cv);
}

void hv_virtio_fs_poll(HvVirtioFs *d) {
    if (!d) return;
    uint32_t done = __atomic_load_n(&d->completed, __ATOMIC_ACQUIRE);
    if (d->published == done) return;
    int full = d->submitted - d->published == HV_VFS_RING;
    for (; d->published != done; d->published++) {
        HvVfsReq *r = &d->ring[d->published % HV_VFS_RING];
        hv_virtq_push(&d->vio, r->q, &r->e, r->written);
    }
    hv_virtio_interrupt(&d->vio);
    if (full) /* requests that waited for room */
        for (int q = 0; q < d->vio.nq; q++) notify(&d->vio, q);
}

void hv_virtio_fs_drain(HvVirtioFs *d) {
    while (__atomic_load_n(&d->completed, __ATOMIC_ACQUIRE) != d->submitted) usleep(100);
    hv_virtio_fs_poll(d);
}

HvVirtioFs *hv_virtio_fs_create(HvMachine *m, HvPcie *bus, const char *tag) {
    HvVirtioFs *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->ring = calloc(HV_VFS_RING, sizeof *d->ring);
    d->in = malloc(HV_VFS_BUF);
    d->out = malloc(HV_VFS_BUF);
    if (!d->ring || !d->in || !d->out) {
        free(d->ring), free(d->in), free(d->out), free(d);
        return NULL;
    }
    hv_fuse_init(&d->fuse);
    HvVirtio *v = &d->vio;
    hv_virtio_init(v, m, 26, 0x018000, 2, 0); /* type 26; a high-priority queue and one request queue */
    v->dev = d;
    v->notify = notify;
    strncpy((char *)v->config, tag, 36); /* char tag[36], not NUL-terminated when full */
    uint32_t request_queues = 1;
    memcpy(v->config + 36, &request_queues, 4);
    v->config_len = 40;
    /* Every file the Mac holds open is a descriptor here; iOS starts apps with a low limit. */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 4096) {
        rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    pthread_mutex_init(&d->mu, NULL);
    pthread_cond_init(&d->cv, NULL);
    pthread_create(&d->worker, NULL, worker, d);
    hv_pcie_attach(bus, &v->pci);
    return d;
}
