/* What HFS+ and APFS share: finding the volume, paths, compressed files
   (decmpfs) and copying a folder out. */
#include <compression.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tl_dmg_private.h"

int tl_vol_read(TlVolume *vol, uint64_t off, void *buf, size_t len) { return tl_disk_read(vol->disk, vol->base + off, buf, len); }

static int probe(TlDisk *disk, uint64_t off) {
    uint8_t b[1100];
    if (tl_disk_read(disk, off, b, sizeof b)) return 0;
    if (!memcmp(b + 32, "NXSB", 4)) return 2;
    if ((b[1024] == 'H' && (b[1025] == '+' || b[1025] == 'X'))) return 1;
    return 0;
}

TlVolume *tl_volume_open(TlDisk *disk, char *err, size_t errlen) {
    int n = tl_disk_partition_count(disk);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            uint64_t off = tl_disk_partition_offset(disk, i);
            int kind = probe(disk, off);
            if (kind == 2 && pass == 0) return tl_apfs_open(disk, off, err, errlen);
            if (kind == 1 && pass == 1) return tl_hfs_open(disk, off, err, errlen);
        }
    /* A partition map the image does not split out: look at GPT's usual first partition. */
    if (probe(disk, 40 * 512) == 2) return tl_apfs_open(disk, 40 * 512, err, errlen);
    if (probe(disk, 40 * 512) == 1) return tl_hfs_open(disk, 40 * 512, err, errlen);
    tl_set_err(err, errlen, "no HFS+ or APFS volume in this image");
    return NULL;
}

void tl_volume_close(TlVolume *vol) {
    if (!vol) return;
    free(vol->z_data);
    vol->ops->close(vol);
}

const char *tl_volume_kind(const TlVolume *vol) { return vol->ops->kind; }
const char *tl_volume_name(const TlVolume *vol) { return vol->name; }
int tl_volume_list(TlVolume *vol, const TlNode *dir, TlListVisitor visit) { return vol->ops->list(vol, dir, visit); }
ssize_t tl_volume_readlink(TlVolume *vol, const TlNode *link, char *buf, size_t len) { return vol->ops->readlink(vol, link, buf, len); }

int tl_volume_lookup(TlVolume *vol, const char *path, TlNode *out) {
    TlNode cur = vol->root;
    char part[1024];
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t n = strcspn(p, "/");
        if (n >= sizeof part) return -1;
        memcpy(part, p, n);
        part[n] = 0;
        p += n;
        if (!S_ISDIR(cur.mode)) return -1;
        __block int found = 0;
        __block TlNode next;
        const char *want = part;
        vol->ops->list(vol, &cur, ^int(const char *name, const TlNode *node) {
            if (strcmp(name, want)) return 0;
            next = *node, found = 1;
            return 1;
        });
        if (!found) return -1;
        cur = next;
    }
    *out = cur;
    return 0;
}

/* ---- compressed files (decmpfs) ---- */

typedef struct __attribute__((packed)) DecmpfsHeader {
    uint32_t magic; /* 'fpmc' */
    uint32_t type;
    uint64_t size;
} DecmpfsHeader;

int64_t tl_decmpfs_size(TlVolume *vol, const TlNode *node) {
    DecmpfsHeader h;
    if (vol->ops->get_xattr(vol, node, "com.apple.decmpfs", &h, sizeof h) < (ssize_t)sizeof h) return -1;
    if (h.magic != 0x636d7066) return -1;
    return (int64_t)h.size;
}

/* Raw LZVN, which libcompression reads inside an LZFSE stream's "bvxn" block. */
static int lzvn_decode(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen) {
    size_t wrapped_len = inlen + 16;
    uint8_t *w = malloc(wrapped_len);
    if (!w) return -1;
    uint32_t hdr[3] = {0x6e787662 /* bvxn */, (uint32_t)outlen, (uint32_t)inlen};
    memcpy(w, hdr, 12);
    memcpy(w + 12, in, inlen);
    memcpy(w + 12 + inlen, "bvx$", 4);
    size_t got = compression_decode_buffer(out, outlen, w, wrapped_len, NULL, COMPRESSION_LZFSE);
    free(w);
    return got == outlen ? 0 : -1;
}

static int decode_block(uint32_t type, const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen) {
    switch (type) {
    case 3: case 4: /* zlib; a first byte of 0xFF (low nibble 0xF) means stored */
        if (inlen && (in[0] & 0x0f) == 0x0f) {
            if (inlen - 1 < outlen) return -1;
            memcpy(out, in + 1, outlen);
            return 0;
        }
        return tl_inflate(in, inlen, out, outlen, 0);
    case 7: case 8: /* LZVN; 0x06 first means stored */
        if (inlen && in[0] == 0x06) {
            if (inlen - 1 < outlen) return -1;
            memcpy(out, in + 1, outlen);
            return 0;
        }
        return lzvn_decode(in, inlen, out, outlen);
    case 11: case 12: /* LZFSE; 0xFF first means stored */
        if (inlen && in[0] == 0xff) {
            if (inlen - 1 < outlen) return -1;
            memcpy(out, in + 1, outlen);
            return 0;
        }
        return compression_decode_buffer(out, outlen, in, inlen, NULL, COMPRESSION_LZFSE) == outlen ? 0 : -1;
    case 1: case 9: case 10:
        if (inlen < outlen) return -1;
        memcpy(out, in, outlen);
        return 0;
    default: return -1;
    }
}

static int decompress_whole(TlVolume *vol, const TlNode *node) {
    if (vol->z_data && vol->z_id == node->id) return 0;
    ssize_t xl = vol->ops->get_xattr(vol, node, "com.apple.decmpfs", NULL, 0);
    if (xl < (ssize_t)sizeof(DecmpfsHeader)) return -1;
    uint8_t *x = malloc((size_t)xl);
    if (!x || vol->ops->get_xattr(vol, node, "com.apple.decmpfs", x, (size_t)xl) != xl) { free(x); return -1; }
    DecmpfsHeader h;
    memcpy(&h, x, sizeof h);
    if (h.magic != 0x636d7066 || h.size > (1ull << 32)) { free(x); return -1; }
    uint8_t *out = malloc(h.size ? h.size : 1);
    int rc = -1;
    if (!out) goto done;
    if (h.type == 3 || h.type == 7 || h.type == 11 || h.type == 1 || h.type == 9) {
        /* Inline: the data follows the header in the attribute. */
        rc = decode_block(h.type, x + sizeof h, (size_t)xl - sizeof h, out, h.size);
    } else if (h.type == 4 || h.type == 8 || h.type == 12 || h.type == 10) {
        uint64_t rlen = vol->ops->fork_size(vol, node, 1);
        uint8_t *r = malloc(rlen ? rlen : 1);
        if (!r || vol->ops->read_fork(vol, node, 1, 0, r, rlen) != (ssize_t)rlen) { free(r); goto done; }
        rc = 0;
        uint64_t nblocks = (h.size + 0xffff) / 0x10000;
        if (h.type == 4) {
            /* A classic resource fork: data at 256, then a count and (offset, size) pairs from 260. */
            if (rlen < 260 + 4) rc = -1;
            const uint8_t *tbl = r + 260;
            uint32_t count;
            memcpy(&count, tbl, 4);
            for (uint32_t i = 0; rc == 0 && i < count && i < nblocks; i++) {
                uint32_t bo, bs;
                memcpy(&bo, tbl + 4 + 8 * i, 4);
                memcpy(&bs, tbl + 8 + 8 * i, 4);
                uint64_t ulen = h.size - i * 0x10000ull < 0x10000 ? h.size - i * 0x10000ull : 0x10000;
                if (260 + (uint64_t)bo + bs > rlen) { rc = -1; break; }
                rc = decode_block(4, r + 260 + bo, bs, out + i * 0x10000ull, (size_t)ulen);
            }
        } else if (h.type == 10) {
            rc = rlen >= h.size ? (memcpy(out, r, h.size), 0) : -1;
        } else {
            /* LZVN and LZFSE: a table of nblocks+1 offsets from the fork's start. */
            for (uint64_t i = 0; rc == 0 && i < nblocks; i++) {
                uint32_t a, b;
                if ((i + 2) * 4 > rlen) { rc = -1; break; }
                memcpy(&a, r + 4 * i, 4);
                memcpy(&b, r + 4 * (i + 1), 4);
                uint64_t ulen = h.size - i * 0x10000ull < 0x10000 ? h.size - i * 0x10000ull : 0x10000;
                if (b < a || b > rlen) { rc = -1; break; }
                rc = decode_block(h.type, r + a, b - a, out + i * 0x10000ull, (size_t)ulen);
            }
        }
        free(r);
    }
done:
    free(x);
    if (rc) { free(out); return -1; }
    free(vol->z_data);
    vol->z_data = out, vol->z_len = h.size, vol->z_id = node->id;
    return 0;
}

ssize_t tl_volume_read(TlVolume *vol, const TlNode *node, uint64_t off, void *buf, size_t len) {
    if (!S_ISREG(node->mode)) return -1;
    if (node->flags & 0x20 /* UF_COMPRESSED */) {
        if (decompress_whole(vol, node)) return -1;
        if (off >= vol->z_len) return 0;
        size_t n = (size_t)(vol->z_len - off < len ? vol->z_len - off : len);
        memcpy(buf, vol->z_data + off, n);
        return (ssize_t)n;
    }
    return vol->ops->read_fork(vol, node, 0, off, buf, len);
}

/* ---- copying out ---- */

typedef struct Linked {
    uint64_t id;
    char path[PATH_MAX];
} Linked;

typedef struct Extractor {
    TlVolume *vol;
    char *err;
    size_t errlen;
    Linked *links;
    size_t nlinks, cap;
    uint8_t *buf;
} Extractor;

static int extract_node(Extractor *x, const TlNode *node, const char *dest);

static int extract_file(Extractor *x, const TlNode *node, const char *dest) {
    if (node->nlink > 1) {
        for (size_t i = 0; i < x->nlinks; i++)
            if (x->links[i].id == node->id) return link(x->links[i].path, dest) ? -1 : 0;
    }
    int fd = open(dest, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { tl_set_err(x->err, x->errlen, "cannot create %s: %s", dest, strerror(errno)); return -1; }
    uint64_t off = 0;
    for (;;) {
        ssize_t n = tl_volume_read(x->vol, node, off, x->buf, 1 << 20);
        if (n < 0) { close(fd); tl_set_err(x->err, x->errlen, "cannot read %s from the image", dest); return -1; }
        if (n == 0) break;
        if (write(fd, x->buf, (size_t)n) != n) { close(fd); tl_set_err(x->err, x->errlen, "cannot write %s", dest); return -1; }
        off += (uint64_t)n;
    }
    fchmod(fd, node->mode & 07777 & ~(S_ISUID | S_ISGID));
    close(fd);
    if (off != node->size) { tl_set_err(x->err, x->errlen, "%s: read %llu of %llu bytes", dest, (unsigned long long)off, (unsigned long long)node->size); return -1; }
    if (node->nlink > 1) {
        if (x->nlinks == x->cap) {
            x->cap = x->cap ? x->cap * 2 : 64;
            x->links = realloc(x->links, x->cap * sizeof *x->links);
        }
        x->links[x->nlinks].id = node->id;
        snprintf(x->links[x->nlinks++].path, PATH_MAX, "%s", dest);
    }
    return 0;
}

static int extract_node(Extractor *x, const TlNode *node, const char *dest) {
    if (S_ISDIR(node->mode)) {
        if (mkdir(dest, 0700) && errno != EEXIST) { tl_set_err(x->err, x->errlen, "cannot make %s: %s", dest, strerror(errno)); return -1; }
        __block int rc = 0;
        tl_volume_list(x->vol, node, ^int(const char *name, const TlNode *child) {
            if (!strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/') || !*name) return 0;
            char path[PATH_MAX];
            if (snprintf(path, sizeof path, "%s/%s", dest, name) >= (int)sizeof path) { rc = -1; return 1; }
            if (extract_node(x, child, path)) { rc = -1; return 1; }
            return 0;
        });
        chmod(dest, (node->mode & 07777) | 0700);
        return rc;
    }
    if (S_ISLNK(node->mode)) {
        char target[PATH_MAX];
        ssize_t n = tl_volume_readlink(x->vol, node, target, sizeof target - 1);
        if (n < 0) { tl_set_err(x->err, x->errlen, "cannot read the link %s", dest); return -1; }
        target[n] = 0;
        if (symlink(target, dest)) { tl_set_err(x->err, x->errlen, "cannot make the link %s: %s", dest, strerror(errno)); return -1; }
        return 0;
    }
    if (S_ISREG(node->mode)) return extract_file(x, node, dest);
    return 0; /* devices, sockets and FIFOs are skipped */
}

int tl_volume_extract(TlVolume *vol, const char *path, const char *dest, char *err, size_t errlen) {
    TlNode node;
    if (tl_volume_lookup(vol, path, &node)) { tl_set_err(err, errlen, "%s is not in the image", path); return -1; }
    Extractor x = {vol, err, errlen, NULL, 0, 0, malloc(1 << 20)};
    int rc = extract_node(&x, &node, dest);
    free(x.links);
    free(x.buf);
    return rc;
}

void tl_utf16be_to_utf8(const uint8_t *in, size_t units, char *out, size_t outlen) {
    size_t o = 0;
    for (size_t i = 0; i < units && o + 5 < outlen; i++) {
        uint32_t c = (uint32_t)in[2 * i] << 8 | in[2 * i + 1];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < units) {
            uint32_t lo = (uint32_t)in[2 * i + 2] << 8 | in[2 * i + 3];
            if (lo >= 0xdc00 && lo < 0xe000) c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00), i++;
        }
        if (c == '/') c = ':'; /* HFS+ keeps a POSIX ':' as '/' */
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) out[o++] = (char)(0xc0 | c >> 6), out[o++] = (char)(0x80 | (c & 0x3f));
        else if (c < 0x10000) out[o++] = (char)(0xe0 | c >> 12), out[o++] = (char)(0x80 | ((c >> 6) & 0x3f)), out[o++] = (char)(0x80 | (c & 0x3f));
        else out[o++] = (char)(0xf0 | c >> 18), out[o++] = (char)(0x80 | ((c >> 12) & 0x3f)), out[o++] = (char)(0x80 | ((c >> 6) & 0x3f)), out[o++] = (char)(0x80 | (c & 0x3f));
    }
    out[o] = 0;
}
