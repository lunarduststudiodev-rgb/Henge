/* APFS, read-only: the newest checkpoint's container superblock, the object
   maps, and the first volume's file-system tree read whole into memory
   (inodes, directory records, extents and extended attributes). Encrypted
   volumes are refused. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "tl_dmg_private.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

enum { J_INODE = 3, J_XATTR = 4, J_EXTENT = 8, J_DREC = 9 };

typedef struct Inode {
    uint64_t id, private_id, size;
    uint32_t flags, uid, gid, nlink;
    uint16_t mode;
} Inode;

typedef struct Drec {
    uint64_t parent, file_id;
    char *name;
} Drec;

typedef struct Ext {
    uint64_t owner, logical, len, phys;
} Ext;

typedef struct Xattr {
    uint64_t owner;
    char *name;
    uint16_t flags;
    uint8_t *data; /* embedded */
    uint32_t data_len;
    uint64_t stream_id, stream_size;
} Xattr;

typedef struct Apfs {
    uint32_t bs;
    uint64_t xid;
    uint64_t container_omap_tree, volume_omap_tree;
    int hashed_names;
    Inode *inodes; size_t ninodes, capi;
    Drec *drecs; size_t ndrecs, capd;
    Ext *exts; size_t nexts, cape;
    Xattr *xattrs; size_t nxattrs, capx;
} Apfs;

static int read_block(TlVolume *vol, uint64_t block, uint8_t *buf) {
    Apfs *a = vol->impl;
    return tl_vol_read(vol, block * a->bs, buf, a->bs);
}

/* B-tree node: a table of contents, then keys growing up and values growing down. */
typedef struct Node {
    uint16_t flags, level;
    uint32_t nkeys;
    const uint8_t *toc, *keys, *vals_end;
    int fixed;
} Node;

static int parse_node(Apfs *a, const uint8_t *b, Node *n) {
    n->flags = le16(b + 32), n->level = le16(b + 34), n->nkeys = le32(b + 36);
    uint16_t toff = le16(b + 40), tlen = le16(b + 42);
    n->toc = b + 56 + toff;
    n->keys = b + 56 + toff + tlen;
    n->vals_end = b + a->bs - ((n->flags & 1) ? 40 : 0); /* a root node ends with btree_info */
    n->fixed = (n->flags & 4) != 0;
    if (56 + (uint32_t)toff + tlen > a->bs) return -1;
    return 0;
}

static void node_kv(const Node *n, uint32_t i, const uint8_t **k, uint16_t *klen, const uint8_t **v, uint16_t *vlen) {
    if (n->fixed) {
        const uint8_t *e = n->toc + 4 * i;
        *k = n->keys + le16(e), *klen = 16;
        uint16_t vo = le16(e + 2);
        *v = vo == 0xffff ? NULL : n->vals_end - vo, *vlen = 16;
    } else {
        const uint8_t *e = n->toc + 8 * i;
        *k = n->keys + le16(e), *klen = le16(e + 2);
        uint16_t vo = le16(e + 4);
        *v = vo == 0xffff ? NULL : n->vals_end - vo, *vlen = le16(e + 6);
    }
}

/* Object map: a virtual object id to its block, as of the newest transaction. */
static uint64_t omap_lookup(TlVolume *vol, uint64_t tree, uint64_t oid) {
    Apfs *a = vol->impl;
    uint8_t *b = malloc(a->bs);
    uint64_t block = tree, found = 0;
    for (int depth = 0; depth < 16 && b; depth++) {
        if (read_block(vol, block, b)) break;
        Node n;
        if (parse_node(a, b, &n)) break;
        /* The last key at or below (oid, xid). */
        int64_t best = -1;
        for (uint32_t i = 0; i < n.nkeys; i++) {
            const uint8_t *k, *v;
            uint16_t kl, vl;
            node_kv(&n, i, &k, &kl, &v, &vl);
            uint64_t koid = le64(k), kxid = le64(k + 8);
            if (koid < oid || (koid == oid && kxid <= a->xid)) best = i;
            else break;
        }
        if (best < 0) break;
        const uint8_t *k, *v;
        uint16_t kl, vl;
        node_kv(&n, (uint32_t)best, &k, &kl, &v, &vl);
        if (!v) break;
        if (n.level == 0) {
            if (le64(k) == oid) found = le64(v + 8);
            break;
        }
        block = le64(v);
    }
    free(b);
    return found;
}

#define PUSH(arr, n, cap) ((n) == (cap) ? ((arr) = realloc((arr), ((cap) = (cap) ? (cap) * 2 : 256) * sizeof *(arr))) : 0, &(arr)[(n)++])

static void add_record(Apfs *a, const uint8_t *k, uint16_t klen, const uint8_t *v, uint16_t vlen) {
    uint64_t hdr = le64(k);
    uint64_t oid = hdr & 0x0fffffffffffffffull;
    int type = (int)(hdr >> 60);
    if (type == J_INODE && vlen >= 92) {
        Inode *in = PUSH(a->inodes, a->ninodes, a->capi);
        memset(in, 0, sizeof *in);
        in->id = oid;
        in->private_id = le64(v + 8);
        in->nlink = le32(v + 56);
        in->flags = le32(v + 68), in->uid = le32(v + 72), in->gid = le32(v + 76);
        in->mode = le16(v + 80);
        /* Extended fields: the data stream's size. */
        if (vlen >= 96) {
            uint16_t nx = le16(v + 92);
            const uint8_t *desc = v + 96, *data = v + 96 + 4 * nx;
            for (uint16_t i = 0; i < nx && data < v + vlen; i++) {
                uint8_t xt = desc[4 * i];
                uint16_t xs = le16(desc + 4 * i + 2);
                if (xt == 8 /* INO_EXT_TYPE_DSTREAM */ && xs >= 8) in->size = le64(data);
                data += (xs + 7) & ~7u;
            }
        }
        if (!S_ISDIR(in->mode)) in->nlink = in->nlink ? in->nlink : 1;
        else in->nlink = 1;
    } else if (type == J_DREC) {
        Drec *d = PUSH(a->drecs, a->ndrecs, a->capd);
        const char *name;
        size_t nlen;
        if (a->hashed_names) nlen = le32(k + 8) & 0x3ff, name = (const char *)k + 12;
        else nlen = le16(k + 8), name = (const char *)k + 10;
        d->parent = oid;
        d->name = strndup(name, nlen);
        d->file_id = le64(v);
    } else if (type == J_EXTENT && vlen >= 16) {
        Ext *e = PUSH(a->exts, a->nexts, a->cape);
        e->owner = oid;
        e->logical = le64(k + 8);
        e->len = le64(v) & 0x00ffffffffffffffull;
        e->phys = le64(v + 8);
    } else if (type == J_XATTR && vlen >= 4) {
        Xattr *x = PUSH(a->xattrs, a->nxattrs, a->capx);
        memset(x, 0, sizeof *x);
        x->owner = oid;
        x->name = strndup((const char *)k + 10, le16(k + 8));
        x->flags = le16(v);
        uint16_t dl = le16(v + 2);
        if (x->flags & 2) { /* embedded */
            x->data = malloc(dl ? dl : 1);
            memcpy(x->data, v + 4, dl);
            x->data_len = dl;
        } else if (dl >= 16) { /* a stream: its object id and size */
            x->stream_id = le64(v + 4);
            x->stream_size = le64(v + 12);
        }
    }
}

static int walk_fs_tree(TlVolume *vol, uint64_t block, int depth) {
    Apfs *a = vol->impl;
    if (depth > 16) return -1;
    uint8_t *b = malloc(a->bs);
    if (!b || read_block(vol, block, b)) { free(b); return -1; }
    Node n;
    if (parse_node(a, b, &n)) { free(b); return -1; }
    int rc = 0;
    for (uint32_t i = 0; i < n.nkeys && !rc; i++) {
        const uint8_t *k, *v;
        uint16_t kl, vl;
        node_kv(&n, i, &k, &kl, &v, &vl);
        if (!v) continue;
        if (n.level == 0) {
            add_record(a, k, kl, v, vl);
        } else {
            uint64_t child = omap_lookup(vol, a->volume_omap_tree, le64(v));
            rc = child ? walk_fs_tree(vol, child, depth + 1) : -1;
        }
    }
    free(b);
    return rc;
}

static int cmp_inode(const void *x, const void *y) {
    const Inode *a = x, *b = y;
    return a->id < b->id ? -1 : a->id > b->id;
}
static int cmp_drec(const void *x, const void *y) {
    const Drec *a = x, *b = y;
    return a->parent < b->parent ? -1 : a->parent > b->parent ? 1 : strcmp(a->name, b->name);
}
static int cmp_ext(const void *x, const void *y) {
    const Ext *a = x, *b = y;
    return a->owner < b->owner ? -1 : a->owner > b->owner ? 1 : a->logical < b->logical ? -1 : a->logical > b->logical;
}

static const Inode *inode(Apfs *a, uint64_t id) {
    size_t lo = 0, hi = a->ninodes;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (a->inodes[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < a->ninodes && a->inodes[lo].id == id ? &a->inodes[lo] : NULL;
}

static void to_node(TlVolume *vol, const Inode *in, TlNode *n) {
    memset(n, 0, sizeof *n);
    n->id = in->id, n->mode = in->mode, n->uid = in->uid, n->gid = in->gid, n->flags = in->flags;
    n->size = in->size, n->nlink = in->nlink;
    if (n->flags & 0x20) {
        int64_t z = tl_decmpfs_size(vol, n);
        if (z >= 0) n->size = (uint64_t)z;
    }
}

static int apfs_list(TlVolume *vol, const TlNode *dir, TlListVisitor visit) {
    Apfs *a = vol->impl;
    size_t lo = 0, hi = a->ndrecs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (a->drecs[mid].parent < dir->id) lo = mid + 1;
        else hi = mid;
    }
    for (size_t i = lo; i < a->ndrecs && a->drecs[i].parent == dir->id; i++) {
        const Inode *in = inode(a, a->drecs[i].file_id);
        if (!in) continue;
        TlNode n;
        to_node(vol, in, &n);
        if (visit(a->drecs[i].name, &n)) return 1;
    }
    return 0;
}

/* Bytes of a data stream (a file's, or a big attribute's) through its extents. */
static ssize_t stream_read(TlVolume *vol, uint64_t owner, uint64_t size, uint64_t off, void *buf, size_t len) {
    Apfs *a = vol->impl;
    if (off >= size) return 0;
    if (len > size - off) len = (size_t)(size - off);
    size_t lo = 0, hi = a->nexts;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (a->exts[mid].owner < owner) lo = mid + 1;
        else hi = mid;
    }
    uint8_t *out = buf;
    size_t done = 0;
    for (size_t i = lo; i < a->nexts && a->exts[i].owner == owner && done < len; i++) {
        const Ext *e = &a->exts[i];
        uint64_t want = off + done;
        if (want < e->logical || want >= e->logical + e->len) continue;
        uint64_t in_ext = want - e->logical;
        size_t n = (size_t)(e->len - in_ext < len - done ? e->len - in_ext : len - done);
        if (!e->phys) memset(out + done, 0, n); /* sparse */
        else if (tl_vol_read(vol, e->phys * a->bs + in_ext, out + done, n)) return -1;
        done += n;
    }
    if (done < len) memset(out + done, 0, len - done), done = len; /* a hole at the end */
    return (ssize_t)done;
}

static const Xattr *find_xattr(Apfs *a, uint64_t owner, const char *name) {
    for (size_t i = 0; i < a->nxattrs; i++)
        if (a->xattrs[i].owner == owner && !strcmp(a->xattrs[i].name, name)) return &a->xattrs[i];
    return NULL;
}

static ssize_t apfs_get_xattr(TlVolume *vol, const TlNode *n, const char *name, void *buf, size_t len) {
    Apfs *a = vol->impl;
    const Xattr *x = find_xattr(a, n->id, name);
    if (!x) return -1;
    if (x->flags & 2) {
        if (buf) memcpy(buf, x->data, x->data_len < len ? x->data_len : len);
        return x->data_len;
    }
    if (buf && stream_read(vol, x->stream_id, x->stream_size, 0, buf, len < x->stream_size ? len : (size_t)x->stream_size) < 0) return -1;
    return (ssize_t)x->stream_size;
}

static ssize_t apfs_read_fork(TlVolume *vol, const TlNode *n, int resource, uint64_t off, void *buf, size_t len) {
    Apfs *a = vol->impl;
    if (resource) {
        const Xattr *x = find_xattr(a, n->id, "com.apple.ResourceFork");
        if (!x) return -1;
        if (x->flags & 2) {
            if (off >= x->data_len) return 0;
            size_t c = x->data_len - off < len ? x->data_len - (size_t)off : len;
            memcpy(buf, x->data + off, c);
            return (ssize_t)c;
        }
        return stream_read(vol, x->stream_id, x->stream_size, off, buf, len);
    }
    const Inode *in = inode(a, n->id);
    if (!in) return -1;
    return stream_read(vol, in->private_id, in->size, off, buf, len);
}

static uint64_t apfs_fork_size(TlVolume *vol, const TlNode *n, int resource) {
    Apfs *a = vol->impl;
    if (resource) {
        const Xattr *x = find_xattr(a, n->id, "com.apple.ResourceFork");
        return !x ? 0 : (x->flags & 2) ? x->data_len : x->stream_size;
    }
    const Inode *in = inode(a, n->id);
    return in ? in->size : 0;
}

static ssize_t apfs_readlink(TlVolume *vol, const TlNode *n, char *buf, size_t len) {
    ssize_t r = apfs_get_xattr(vol, n, "com.apple.fs.symlink", buf, len);
    if (r < 0) return -1;
    if ((size_t)r > len) r = (ssize_t)len;
    while (r > 0 && buf[r - 1] == 0) r--; /* stored with its NUL */
    return r;
}

static void apfs_close(TlVolume *vol) {
    Apfs *a = vol->impl;
    for (size_t i = 0; i < a->ndrecs; i++) free(a->drecs[i].name);
    for (size_t i = 0; i < a->nxattrs; i++) free(a->xattrs[i].name), free(a->xattrs[i].data);
    free(a->inodes), free(a->drecs), free(a->exts), free(a->xattrs);
    free(a);
    free(vol);
}

static const TlVolOps apfs_ops = {"APFS", apfs_list, apfs_read_fork, apfs_fork_size, apfs_get_xattr, apfs_readlink, apfs_close};

TlVolume *tl_apfs_open(TlDisk *disk, uint64_t base, char *err, size_t errlen) {
    TlVolume *vol = calloc(1, sizeof *vol);
    Apfs *a = calloc(1, sizeof *a);
    vol->ops = &apfs_ops, vol->disk = disk, vol->base = base, vol->impl = a;
    uint8_t sb0[4096];
    if (tl_vol_read(vol, 0, sb0, sizeof sb0) || memcmp(sb0 + 32, "NXSB", 4)) { tl_set_err(err, errlen, "no APFS container"); apfs_close(vol); return NULL; }
    a->bs = le32(sb0 + 36);
    if (a->bs < 4096 || a->bs > 65536) { tl_set_err(err, errlen, "bad APFS block size"); apfs_close(vol); return NULL; }
    uint8_t *sb = malloc(a->bs), *b = malloc(a->bs);
    memcpy(sb, sb0, sizeof sb0 < a->bs ? sizeof sb0 : a->bs);
    if (a->bs > sizeof sb0) read_block(vol, 0, sb);
    /* The newest superblock in the checkpoint descriptor area. */
    uint32_t desc_blocks = le32(sb + 104);
    uint64_t desc_base = le64(sb + 112);
    a->xid = le64(sb + 16);
    if (!(desc_blocks & 0x80000000u))
        for (uint32_t i = 0; i < desc_blocks; i++) {
            if (read_block(vol, desc_base + i, b)) break;
            if ((le32(b + 24) & 0xffff) == 1 && !memcmp(b + 32, "NXSB", 4) && le64(b + 16) > a->xid) {
                memcpy(sb, b, a->bs);
                a->xid = le64(b + 16);
            }
        }
    uint64_t omap = le64(sb + 160), fs_oid = le64(sb + 184);
    int ok = 0;
    if (omap && fs_oid && !read_block(vol, omap, b)) {
        a->container_omap_tree = le64(b + 48);
        uint64_t vsb_block = omap_lookup(vol, a->container_omap_tree, fs_oid);
        if (vsb_block && !read_block(vol, vsb_block, b) && !memcmp(b + 32, "APSB", 4)) {
            uint64_t incompat = le64(b + 56), fs_flags = le64(b + 264);
            a->hashed_names = (incompat & (1 | 8)) != 0;
            snprintf(vol->name, sizeof vol->name, "%.255s", (const char *)b + 704);
            uint64_t vol_omap = le64(b + 128), root_tree = le64(b + 136);
            if (!(fs_flags & 1 /* APFS_FS_UNENCRYPTED */) && !(fs_flags & 8 /* ONEKEY */)) {
                tl_set_err(err, errlen, "the APFS volume is encrypted");
            } else if (!read_block(vol, vol_omap, b)) {
                a->volume_omap_tree = le64(b + 48);
                uint64_t root_block = omap_lookup(vol, a->volume_omap_tree, root_tree);
                if (root_block && !walk_fs_tree(vol, root_block, 0)) ok = 1;
                else tl_set_err(err, errlen, "cannot read the APFS file tree");
            }
        }
    }
    free(sb), free(b);
    if (!ok) {
        if (err && !*err) tl_set_err(err, errlen, "cannot read the APFS volume");
        apfs_close(vol);
        return NULL;
    }
    qsort(a->inodes, a->ninodes, sizeof *a->inodes, cmp_inode);
    qsort(a->drecs, a->ndrecs, sizeof *a->drecs, cmp_drec);
    qsort(a->exts, a->nexts, sizeof *a->exts, cmp_ext);
    const Inode *root = inode(a, 2);
    if (root) to_node(vol, root, &vol->root);
    else vol->root = (TlNode){.id = 2, .mode = S_IFDIR | 0755, .nlink = 1};
    return vol;
}
