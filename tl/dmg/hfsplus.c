/* HFS+ (and HFSX), read-only: the catalog B-tree read whole into memory,
   extents from the fork records and the overflow file, attributes for
   compressed files, and hard links through the private data folder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tl_dmg_private.h"

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

typedef struct Extent { uint32_t start, count; } Extent;

typedef struct Fork {
    uint64_t size;
    uint32_t blocks;
    Extent ext[8];
} Fork;

typedef struct Entry {
    uint32_t parent;
    char name[768];
    int16_t type;         /* 1 folder, 2 file */
    uint32_t id;
    uint32_t uid, gid;
    uint8_t admin_flags, owner_flags;
    uint16_t mode;
    uint32_t special;     /* hard links: the iNode number */
    uint32_t fd_type, fd_creator;
    Fork data, rsrc;
} Entry;

typedef struct Tree {
    Fork fork;
    uint32_t file_id;     /* 3 extents, 4 catalog, 8 attributes */
    uint16_t node_size;
    uint32_t root, first_leaf;
    uint8_t *cache;       /* the whole tree file, read once */
} Tree;

typedef struct Hfs {
    uint32_t block_size;
    Tree extents, catalog, attributes;
    Entry *entries;       /* sorted by (parent, name) as the catalog is */
    size_t nentries;
    uint32_t private_dir; /* "\0\0\0\0HFS+ Private Data" */
} Hfs;

static void read_fork_record(const uint8_t *p, Fork *f) {
    f->size = be64(p);
    f->blocks = be32(p + 12);
    for (int i = 0; i < 8; i++) f->ext[i].start = be32(p + 16 + 8 * i), f->ext[i].count = be32(p + 20 + 8 * i);
}

static int tree_load(TlVolume *vol, Hfs *h, Tree *t);

/* The extents of a fork, overflow file included. Returns how many; *out is malloc'd. */
static size_t fork_extents(TlVolume *vol, Hfs *h, const Fork *f, uint32_t file_id, int resource, Extent **out) {
    size_t n = 0, cap = 16;
    Extent *e = malloc(cap * sizeof *e);
    uint32_t have = 0;
    for (int i = 0; i < 8 && f->ext[i].count; i++) e[n++] = f->ext[i], have += f->ext[i].count;
    if (have < f->blocks && file_id != 3 && h->extents.cache) {
        /* Overflow records: key (forkType, fileID, startBlock) -> 8 more extents. */
        Tree *t = &h->extents;
        for (uint32_t node = t->first_leaf; node && have < f->blocks;) {
            const uint8_t *nd = t->cache + (uint64_t)node * t->node_size;
            uint16_t nrec = be16(nd + 10);
            for (uint16_t r = 0; r < nrec; r++) {
                uint16_t off = be16(nd + t->node_size - 2 * (r + 1));
                const uint8_t *k = nd + off;
                uint16_t klen = be16(k);
                if (k[2] != (resource ? 0xff : 0) || be32(k + 4) != file_id || be32(k + 8) != have) continue;
                const uint8_t *rec = k + 2 + klen;
                for (int i = 0; i < 8; i++) {
                    Extent x = {be32(rec + 8 * i), be32(rec + 4 + 8 * i)};
                    if (!x.count) break;
                    if (n == cap) e = realloc(e, (cap *= 2) * sizeof *e);
                    e[n++] = x, have += x.count;
                }
            }
            node = be32(nd);
        }
    }
    *out = e;
    return n;
}

static ssize_t fork_read(TlVolume *vol, Hfs *h, const Fork *f, uint32_t file_id, int resource, uint64_t off, void *buf, size_t len) {
    if (off >= f->size) return 0;
    if (len > f->size - off) len = (size_t)(f->size - off);
    Extent *ext;
    size_t n = fork_extents(vol, h, f, file_id, resource, &ext);
    uint8_t *out = buf;
    size_t done = 0;
    uint64_t pos = 0; /* the fork offset where the current extent starts */
    for (size_t i = 0; i < n && done < len; i++) {
        uint64_t elen = (uint64_t)ext[i].count * h->block_size;
        uint64_t want = off + done;
        if (want < pos + elen) {
            uint64_t in_ext = want - pos;
            size_t chunk = (size_t)(elen - in_ext < len - done ? elen - in_ext : len - done);
            if (tl_vol_read(vol, (uint64_t)ext[i].start * h->block_size + in_ext, out + done, chunk)) { free(ext); return -1; }
            done += chunk;
        }
        pos += elen;
    }
    free(ext);
    return (ssize_t)done;
}

static int tree_load(TlVolume *vol, Hfs *h, Tree *t) {
    if (!t->fork.size) return 0;
    t->cache = malloc(t->fork.size);
    if (!t->cache || fork_read(vol, h, &t->fork, t->file_id, 0, 0, t->cache, t->fork.size) != (ssize_t)t->fork.size) return -1;
    const uint8_t *hdr = t->cache + 14;
    t->root = be32(hdr + 2);
    t->first_leaf = be32(hdr + 10);
    t->node_size = be16(hdr + 18);
    if (t->node_size < 512 || (uint64_t)t->node_size > t->fork.size) return -1;
    return 0;
}

static int entry_cmp_id(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    return x->parent < y->parent ? -1 : x->parent > y->parent;
}

static int load_catalog(TlVolume *vol, Hfs *h) {
    Tree *t = &h->catalog;
    size_t cap = 1024;
    h->entries = malloc(cap * sizeof *h->entries);
    uint64_t guard = t->fork.size / t->node_size + 1;
    for (uint32_t node = t->first_leaf; node && guard--;) {
        if ((uint64_t)(node + 1) * t->node_size > t->fork.size) return -1;
        const uint8_t *nd = t->cache + (uint64_t)node * t->node_size;
        if ((int8_t)nd[8] != -1) return -1;
        uint16_t nrec = be16(nd + 10);
        for (uint16_t r = 0; r < nrec; r++) {
            uint16_t off = be16(nd + t->node_size - 2 * (r + 1));
            const uint8_t *k = nd + off;
            uint16_t klen = be16(k);
            const uint8_t *rec = k + 2 + klen;
            int16_t type = (int16_t)be16(rec);
            if (type != 1 && type != 2) continue;
            if (h->nentries == cap) h->entries = realloc(h->entries, (cap *= 2) * sizeof *h->entries);
            Entry *e = &h->entries[h->nentries++];
            memset(e, 0, sizeof *e);
            e->parent = be32(k + 2);
            uint16_t nlen = be16(k + 6);
            if (nlen > 255) nlen = 255;
            tl_utf16be_to_utf8(k + 8, nlen, e->name, sizeof e->name);
            e->type = type;
            e->id = be32(rec + 8);
            e->uid = be32(rec + 32), e->gid = be32(rec + 36);
            e->admin_flags = rec[40], e->owner_flags = rec[41];
            e->mode = be16(rec + 42);
            e->special = be32(rec + 44);
            if (type == 2) {
                e->fd_type = be32(rec + 48), e->fd_creator = be32(rec + 52);
                read_fork_record(rec + 88, &e->data);
                read_fork_record(rec + 168, &e->rsrc);
                if (!(e->mode & S_IFMT)) e->mode |= S_IFREG | 0644;
            } else if (!(e->mode & S_IFMT)) {
                e->mode |= S_IFDIR | 0755;
            }
            if (type == 1 && e->parent == 2 && nlen == 21 && !memcmp(e->name, "\0\0\0\0HFS+ Private Data", 4)) h->private_dir = e->id;
        }
        node = be32(nd);
    }
    /* The catalog is already in (parent, name) order; a stable sort by parent keeps that. */
    qsort(h->entries, h->nentries, sizeof *h->entries, entry_cmp_id);
    return 0;
}

static const Entry *find_by_id(Hfs *h, uint32_t id) {
    for (size_t i = 0; i < h->nentries; i++)
        if (h->entries[i].id == id) return &h->entries[i];
    return NULL;
}

static void to_node(TlVolume *vol, Hfs *h, const Entry *e, TlNode *n) {
    memset(n, 0, sizeof *n);
    n->id = e->id;
    n->mode = e->mode;
    n->uid = e->uid, n->gid = e->gid;
    n->flags = (uint32_t)e->owner_flags | (uint32_t)e->admin_flags << 16;
    n->nlink = 1;
    n->size = e->type == 2 ? e->data.size : 0;
    if (n->flags & 0x20) {
        int64_t z = tl_decmpfs_size(vol, n);
        if (z >= 0) n->size = (uint64_t)z;
    }
}

static int hfs_list(TlVolume *vol, const TlNode *dir, TlListVisitor visit) {
    Hfs *h = vol->impl;
    uint32_t parent = (uint32_t)dir->id;
    /* First entry with this parent. */
    size_t lo = 0, hi = h->nentries;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (h->entries[mid].parent < parent) lo = mid + 1;
        else hi = mid;
    }
    for (size_t i = lo; i < h->nentries && h->entries[i].parent == parent; i++) {
        const Entry *e = &h->entries[i];
        if (parent == 2 && (e->id == h->private_dir || !memcmp(e->name, "\0\0\0\0", 4) || !strcmp(e->name, ".HFS+ Private Directory Data\r") ||
                            !strcmp(e->name, ".journal") || !strcmp(e->name, ".journal_info_block")))
            continue;
        TlNode n;
        if (e->type == 2 && e->fd_type == 0x686c6e6b /* hlnk */ && e->fd_creator == 0x6866732b /* hfs+ */ && h->private_dir) {
            /* A hard link: the file is "iNode<special>" in the private folder. */
            char want[32];
            snprintf(want, sizeof want, "iNode%u", e->special);
            const Entry *target = NULL;
            for (size_t k = 0; k < h->nentries; k++)
                if (h->entries[k].parent == h->private_dir && !strcmp(h->entries[k].name, want)) { target = &h->entries[k]; break; }
            if (!target) continue;
            to_node(vol, h, target, &n);
            n.nlink = 2;
        } else {
            to_node(vol, h, e, &n);
        }
        if (visit(e->name, &n)) return 1;
    }
    return 0;
}

static const Entry *entry_for(Hfs *h, const TlNode *n) { return find_by_id(h, (uint32_t)n->id); }

static ssize_t hfs_read_fork(TlVolume *vol, const TlNode *n, int resource, uint64_t off, void *buf, size_t len) {
    Hfs *h = vol->impl;
    const Entry *e = entry_for(h, n);
    if (!e || e->type != 2) return -1;
    return fork_read(vol, h, resource ? &e->rsrc : &e->data, e->id, resource, off, buf, len);
}

static uint64_t hfs_fork_size(TlVolume *vol, const TlNode *n, int resource) {
    Hfs *h = vol->impl;
    const Entry *e = entry_for(h, n);
    return e && e->type == 2 ? (resource ? e->rsrc.size : e->data.size) : 0;
}

static ssize_t hfs_get_xattr(TlVolume *vol, const TlNode *n, const char *name, void *buf, size_t len) {
    Hfs *h = vol->impl;
    Tree *t = &h->attributes;
    if (!t->cache) return -1;
    for (uint32_t node = t->first_leaf; node;) {
        const uint8_t *nd = t->cache + (uint64_t)node * t->node_size;
        uint16_t nrec = be16(nd + 10);
        for (uint16_t r = 0; r < nrec; r++) {
            uint16_t off = be16(nd + t->node_size - 2 * (r + 1));
            const uint8_t *k = nd + off;
            uint16_t klen = be16(k);
            if (be32(k + 4) != (uint32_t)n->id) continue;
            char aname[512];
            tl_utf16be_to_utf8(k + 14, be16(k + 12), aname, sizeof aname);
            if (strcmp(aname, name)) continue;
            const uint8_t *rec = k + 2 + klen;
            uint32_t type = be32(rec);
            if (type == 0x10) { /* inline data */
                uint32_t size = be32(rec + 12);
                if (buf) memcpy(buf, rec + 16, size < len ? size : len);
                return size;
            }
            if (type == 0x20) { /* in a fork of its own */
                Fork f;
                read_fork_record(rec + 8, &f);
                if (buf) {
                    ssize_t got = fork_read(vol, h, &f, 0, 0, 0, buf, len < f.size ? len : (size_t)f.size);
                    if (got < 0) return -1;
                }
                return (ssize_t)f.size;
            }
            return -1;
        }
        node = be32(nd);
    }
    return -1;
}

static ssize_t hfs_readlink(TlVolume *vol, const TlNode *n, char *buf, size_t len) {
    return hfs_read_fork(vol, n, 0, 0, buf, len);
}

static void hfs_close(TlVolume *vol) {
    Hfs *h = vol->impl;
    free(h->extents.cache);
    free(h->catalog.cache);
    free(h->attributes.cache);
    free(h->entries);
    free(h);
    free(vol);
}

static const TlVolOps hfs_ops = {"HFS+", hfs_list, hfs_read_fork, hfs_fork_size, hfs_get_xattr, hfs_readlink, hfs_close};

TlVolume *tl_hfs_open(TlDisk *disk, uint64_t base, char *err, size_t errlen) {
    TlVolume *vol = calloc(1, sizeof *vol);
    Hfs *h = calloc(1, sizeof *h);
    vol->ops = &hfs_ops, vol->disk = disk, vol->base = base, vol->impl = h;
    uint8_t vh[512];
    if (tl_vol_read(vol, 1024, vh, sizeof vh)) { tl_set_err(err, errlen, "cannot read the HFS+ header"); hfs_close(vol); return NULL; }
    h->block_size = be32(vh + 40);
    if (h->block_size < 512 || (h->block_size & (h->block_size - 1))) { tl_set_err(err, errlen, "bad HFS+ block size"); hfs_close(vol); return NULL; }
    read_fork_record(vh + 192, &h->extents.fork), h->extents.file_id = 3;
    read_fork_record(vh + 272, &h->catalog.fork), h->catalog.file_id = 4;
    read_fork_record(vh + 352, &h->attributes.fork), h->attributes.file_id = 8;
    if (tree_load(vol, h, &h->extents) || tree_load(vol, h, &h->catalog) || tree_load(vol, h, &h->attributes) ||
        !h->catalog.cache || load_catalog(vol, h)) {
        tl_set_err(err, errlen, "cannot read the HFS+ catalog");
        hfs_close(vol);
        return NULL;
    }
    /* The volume's name is the root folder's thread record name; the root's own entry is under parent 1. */
    for (size_t i = 0; i < h->nentries; i++)
        if (h->entries[i].parent == 1 && h->entries[i].id == 2) {
            snprintf(vol->name, sizeof vol->name, "%s", h->entries[i].name);
            to_node(vol, h, &h->entries[i], &vol->root);
        }
    if (!vol->root.id) vol->root = (TlNode){.id = 2, .mode = S_IFDIR | 0755, .nlink = 1};
    return vol;
}
